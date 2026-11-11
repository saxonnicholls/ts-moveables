//
//  encoder.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Byte-level BPE encoding, serial or across a task_pool, token for token what tiktoken gives.
//
//  The pipeline is tiktoken's: split the text into pieces (split.hpp), look each piece up whole, and merge the
//  ones that are not a token with snicholls-core's bpe_encode_piece (tiktoken's byte_pair_merge, step for
//  step). What is different is where the time goes and how the work is cut.
//
//  Cutting. A batch of documents is a list of work items. A document longer than a few chunks is cut at
//  find_boundary positions, places the whole-text split is proved to start a piece, so one long text spreads
//  across every core and still gives exactly its serial tokens. The items go to parallel_for with a grain of
//  one: workers claim them from a shared cursor, so a long item costs one item of imbalance, not one slice.
//  tiktoken and rustbpe parallelise only across documents; a single 100 MB text is one thread to them.
//
//  Output. A range of b bytes gives at most b tokens (every token is at least one byte), so each item writes
//  straight into its own slice of one arena, offset by its first byte: no allocation, no locking and no
//  sharing while encoding. The slices are then copied, in parallel, into each document's vector.
//
//  Threads. The encoder is immutable once built and is shared by every worker; the per-thread state is the
//  merge scratch, which lives on each item's stack frame (grown on the heap only for a piece longer than
//  1 KB).
//

#ifndef ts_moveables_tokenizer_encoder_hpp
#define ts_moveables_tokenizer_encoder_hpp

#include "long_merge.hpp"
#include "split.hpp"
#include "vocab.hpp"

#include <snicholls/core/ml/bpe.hpp>

#include "concurrent/parallel_for.hpp"
#include "interfaces/task_pool.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace snicholls::tokenizer
{

static_assert(no_rank == ml::bpe_no_rank);

class encoder
{
public:
    /// \brief Pieces of this many bytes or more are merged by long_merge (O(n log n)) rather than
    /// bpe_encode_piece (O(n^2)); measured in long_merge_check.
    static constexpr std::size_t long_piece = 1024;

    encoder(tokenizer::vocab v, split_pattern pattern) : vocab_(std::move(v)), pattern_(pattern) {}

    const tokenizer::vocab& vocab() const noexcept { return vocab_; }
    split_pattern pattern() const noexcept { return pattern_; }

    /// \brief Encodes the pieces of `text` that start in [begin, end) into `out`, which must hold end - begin
    /// tokens. `begin` is 0 or a find_boundary. Returns the number of tokens.
    std::size_t encode_range(std::string_view text, std::size_t begin, std::size_t end, std::uint32_t* out) const
    {
        piece_cache& cache = thread_cache();
        if (cache.owner != id_)
            cache.reset(id_);
        std::array<ml::bpe_part, ml::bpe_scratch_size(long_piece - 1)> local;
        std::vector<ml::bpe_part> big;
        std::uint32_t* at = out;
        const auto merge = [&](std::string_view piece) {   // a piece that is not one token
            // A piece of 1 KB or more takes the O(n log n) heap merge (long_merge.hpp), which is at least as
            // fast from there on every shape measured and 100x faster at 128 KB; below, tiktoken's rescanning
            // merge is faster. The two give the same tokens. Long pieces are never cached.
            if (piece.size() >= long_piece) {
                big.resize(long_merge_scratch_size(piece.size()));
                at += take(long_merge(piece, vocab_, std::span(at, piece.size()), std::span(big)));
                return;
            }
            const std::uint64_t h = cache.hash(piece);
            if (const piece_cache::entry* e = cache.find(h, piece)) {
                std::memcpy(at, e->tokens, e->count * sizeof(std::uint32_t));
                at += e->count;
                return;
            }
            const std::size_t count =
                take(ml::bpe_encode_piece(piece, vocab_, std::span(at, piece.size()), std::span(local)));
            cache.insert(h, piece, at, count);
            at += count;
        };

        // Batches of pieces: split a batch and probe each (hash, prefetch its slot), then look them up. 93-98%
        // of pieces are one token and end there; the rest are merged.
        constexpr std::size_t batch = 32;
        std::size_t starts[batch + 1];
        tokenizer::vocab::probe_t probes[batch];
        std::size_t i = begin;
        while (i < end) {
            std::size_t k = 0;
            starts[0] = 0;
            for (; k < batch && i < end; ++k) {
                const std::size_t j = next_piece(text, i, pattern_);
                starts[k] = i - begin;
                probes[k] = vocab_.probe(text.substr(i, j - i));
                i = j;
            }
            starts[k] = i - begin;
            for (std::size_t q = 0; q < k; ++q) {
                const std::string_view piece = text.substr(begin + starts[q], starts[q + 1] - starts[q]);
                if (const std::uint32_t whole = vocab_.find(piece, probes[q]); whole != no_rank)
                    *at++ = whole;
                else
                    merge(piece);
            }
        }
        return static_cast<std::size_t>(at - out);
    }

    /// \brief Encodes `text` on the calling thread.
    std::vector<std::uint32_t> encode(std::string_view text) const
    {
        std::vector<std::uint32_t> out(text.size());
        out.resize(encode_range(text, 0, text.size(), out.data()));
        return out;
    }

    /// \brief Encodes `text` across `pool`, cut into chunks of about `chunk` bytes.
    std::vector<std::uint32_t> encode(std::string_view text, task_pool& pool, std::size_t chunk = 0) const
    {
        std::vector<std::vector<std::uint32_t>> one = encode_batch(std::span(&text, 1), pool, chunk);
        return std::move(one[0]);
    }

    /// \brief Encodes every document across `pool`; long documents are cut into chunks of about `chunk` bytes
    /// (0: chosen from the total and the worker count).
    std::vector<std::vector<std::uint32_t>> encode_batch(std::span<const std::string_view> docs, task_pool& pool,
                                                         std::size_t chunk = 0) const
    {
        struct item {
            std::size_t doc, begin, end, at, count;
        };
        std::size_t total = 0;
        for (const std::string_view d : docs)
            total += d.size();
        if (chunk == 0)
            chunk = std::clamp<std::size_t>(total / (pool.worker_count() * 16 + 1), 16 << 10, 1 << 20);

        // Work items: whole documents, and the long ones cut at proved piece boundaries
        std::vector<item> items;
        items.reserve(docs.size() + total / chunk + 1);
        std::size_t at = 0;
        for (std::size_t d = 0; d < docs.size(); ++d) {
            const std::string_view text = docs[d];
            std::size_t b = 0;
            while (b < text.size()) {
                std::size_t e = text.size();
                if (text.size() - b > chunk + chunk / 2)
                    e = find_boundary(text, b + chunk);
                items.push_back({d, b, e, at + b, 0});
                b = e;
            }
            at += text.size();
        }

        std::unique_ptr<std::uint32_t[]> arena(new std::uint32_t[total ? total : 1]);   // uninitialised
        parallel_for(pool, std::size_t{0}, items.size(), [&](std::size_t k) {
            item& it = items[k];
            it.count = encode_range(docs[it.doc], it.begin, it.end, arena.get() + it.at);
        }, 1);

        // Each document's tokens: its items' slices, in order
        std::vector<std::vector<std::uint32_t>> out(docs.size());
        std::vector<std::size_t> first(docs.size() + 1, items.size());
        for (std::size_t k = items.size(); k-- > 0;)
            first[items[k].doc] = k;
        for (std::size_t d = docs.size(); d-- > 0;)
            if (first[d] == items.size())
                first[d] = first[d + 1];
        parallel_for(pool, std::size_t{0}, docs.size(), [&](std::size_t d) {
            std::size_t n = 0;
            for (std::size_t k = first[d]; k < first[d + 1]; ++k)
                n += items[k].count;
            std::vector<std::uint32_t>& v = out[d];
            v.resize(n);
            std::uint32_t* to = v.data();
            for (std::size_t k = first[d]; k < first[d + 1]; ++k) {
                std::memcpy(to, arena.get() + items[k].at, items[k].count * sizeof(std::uint32_t));
                to += items[k].count;
            }
        });
        return out;
    }

    /// \brief The bytes of `tokens`. Throws std::invalid_argument on an id that is not a token.
    std::string decode(std::span<const std::uint32_t> tokens) const
    {
        std::size_t n = 0;
        for (const std::uint32_t t : tokens) {
            const std::string_view b = vocab_.token(t);
            if (b.empty())
                throw std::invalid_argument("decode: not a token id");
            n += b.size();
        }
        std::string out(n, '\0');
        std::size_t at = 0;
        for (const std::uint32_t t : tokens) {
            const std::string_view b = vocab_.token(t);
            std::memcpy(out.data() + at, b.data(), b.size());
            at += b.size();
        }
        return out;
    }

private:
    // The tokens of recently merged pieces, per thread. A piece that is not one token costs a merge of a few
    // hundred nanoseconds, and in real text the same such pieces recur (195K of them, 41K distinct, in the
    // benchmark corpus), so remembering them is the largest single gain. Direct-mapped, 128-byte entries,
    // 2048 of them: 256 KB, inside one core's L2. Thread-local, so it is never shared and needs no locking; it
    // is tagged with the encoder that filled it, so a thread that moves to another vocabulary starts clean.
    struct piece_cache {
        struct entry {
            std::uint64_t hash;
            std::uint8_t len, count;
            char bytes[46];
            std::uint32_t tokens[18];
        };
        static_assert(sizeof(entry) == 128);
        static constexpr std::size_t size = 2048;

        std::uint64_t owner = 0;
        std::unique_ptr<entry[]> entries{new entry[size]};

        void reset(std::uint64_t id)
        {
            owner = id;
            for (std::size_t i = 0; i < size; ++i)
                entries[i].len = 0;   // len 0 is empty: a cached piece has at least two bytes
        }
        static std::uint64_t hash(std::string_view piece) noexcept { return wyhash(piece); }
        const entry* find(std::uint64_t h, std::string_view piece) const noexcept
        {
            const entry& e = entries[h & (size - 1)];
            if (e.hash == h && e.len == piece.size() && std::memcmp(e.bytes, piece.data(), piece.size()) == 0)
                return &e;
            return nullptr;
        }
        void insert(std::uint64_t h, std::string_view piece, const std::uint32_t* tokens, std::size_t count) noexcept
        {
            if (piece.size() > sizeof(entry::bytes) || count > std::size(entry{}.tokens))
                return;
            entry& e = entries[h & (size - 1)];
            e.hash = h;
            e.len = static_cast<std::uint8_t>(piece.size());
            e.count = static_cast<std::uint8_t>(count);
            std::memcpy(e.bytes, piece.data(), piece.size());
            std::memcpy(e.tokens, tokens, count * sizeof(std::uint32_t));
        }
    };
    static piece_cache& thread_cache()
    {
        thread_local piece_cache cache;
        return cache;
    }
    static std::uint64_t next_id() noexcept
    {
        static std::atomic<std::uint64_t> n{0};
        return n.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    static std::size_t take(const result<std::size_t, ml::bpe_error>& r)
    {
        if (!r)
            throw std::runtime_error("encode: a byte is not in the vocabulary (not a byte-level BPE vocabulary)");
        return *r;
    }

    tokenizer::vocab vocab_;
    split_pattern pattern_;
    std::uint64_t id_ = next_id();
};

} // namespace snicholls::tokenizer

#endif // ts_moveables_tokenizer_encoder_hpp
