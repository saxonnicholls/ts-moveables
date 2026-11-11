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
#include "concepts/task_pool.hpp"

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

#ifndef TOK_MEMO
#define TOK_MEMO 1   // the per-thread memo of short pieces (piece_memo below); 0 builds the batched lookup alone
#endif
#ifndef TOK_DIRECT
#define TOK_DIRECT 1   // one- and two-byte pieces answered from tables built with the encoder
#endif

class encoder
{
public:
    /// \brief Pieces of this many bytes or more are merged by long_merge (O(n log n)) rather than
    /// bpe_encode_piece (O(n^2)); measured in long_merge_check.
    static constexpr std::size_t long_piece = 1024;

    encoder(tokenizer::vocab v, split_pattern pattern) : vocab_(std::move(v)), pattern_(pattern) { build_direct(); }

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

#if TOK_MEMO
        // One pass, through the memo: a piece of up to 15 bytes seen before on this thread is a lookup in a
        // table of 32-byte entries that answers with its tokens, one token or a merged few, without touching
        // the vocabulary. A miss looks the piece up (and merges it if it is not a token) and remembers it.
        // Longer pieces go straight to the vocabulary and the merge cache.
        piece_memo& memo = thread_memo();
        if (memo.owner != id_)
            memo.reset(id_);
        const char* const t = text.data();
        // The tokens of one piece, the memo first for a short one; `set` is its memo set. Forced inline: as a
        // call, `at` and the key go through memory, which measured 5-15% slower. (Prefetching the sets of a
        // batch of 32 pieces before answering them measured no faster, and was removed.)
        const auto one = [&](std::string_view piece, const piece_memo::key& key, piece_memo::entry* set)
#if defined(__GNUC__) || defined(__clang__)
            __attribute__((always_inline))
#endif
        {
            if (piece.size() <= piece_memo::max_bytes) {
                piece_memo::entry& e = piece_memo::way(set, key);
                if (e.same(key)) {
                    // Exactly count tokens: out has room for the piece's bytes, not always for three, and in a
                    // parallel batch the next slot may be another item's, written by another thread
                    at[0] = e.tokens[0];
                    if (e.count > 1) {
                        at[1] = e.tokens[1];
                        if (e.count > 2)
                            at[2] = e.tokens[2];
                    }
                    at += e.count;
                    return;
                }
                std::uint32_t* const first = at;
                if (const std::uint32_t whole = vocab_(piece); whole != no_rank)
                    *at++ = whole;
                else
                    merge(piece);
                memo.remember(e, key, first, static_cast<std::size_t>(at - first));
                return;
            }
            if (const std::uint32_t whole = vocab_(piece); whole != no_rank)
                *at++ = whole;
            else
                merge(piece);
        };
        std::size_t i = begin;
        while (i < end) {
            const std::size_t j = next_piece(text, i, pattern_);
            const std::string_view piece(t + i, j - i);
            i = j;
#if TOK_DIRECT
            // A third of pieces are one or two bytes, and those have answers known in advance (build_direct)
            if (piece.size() <= 2 && !two_byte_.empty()) {
                const auto* u = reinterpret_cast<const unsigned char*>(piece.data());
                if (piece.size() == 1) {
                    *at++ = one_byte_[u[0]];
                } else {
                    const direct2 d = two_byte_[u[0] | (unsigned{u[1]} << 8)];
                    *at++ = d.first;
                    if (d.second != no_rank)
                        *at++ = d.second;
                }
                continue;
            }
#endif
            if (piece.size() <= piece_memo::max_bytes) {
                const piece_memo::key key = piece_memo::key_of(piece);
                one(piece, key, memo.set_of(key));
            } else {
                one(piece, piece_memo::key{}, nullptr);
            }
        }
        return static_cast<std::size_t>(at - out);
#else
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
#endif
    }

    /// \brief Encodes `text` on the calling thread.
    std::vector<std::uint32_t> encode(std::string_view text) const
    {
        std::vector<std::uint32_t> out(text.size());
        out.resize(encode_range(text, 0, text.size(), out.data()));
        return out;
    }

    /// \brief Encodes `text` across `pool` (any task_pool_like: a concrete pool, or a task_pool&), cut into chunks
    /// of about `chunk` bytes.
    template <task_pool_like Pool>
    std::vector<std::uint32_t> encode(std::string_view text, Pool& pool, std::size_t chunk = 0) const
    {
        std::vector<std::vector<std::uint32_t>> one = encode_batch(std::span(&text, 1), pool, chunk);
        return std::move(one[0]);
    }

    /// \brief Encodes every document across `pool`; long documents are cut into chunks of about `chunk` bytes
    /// (0: chosen from the total and the worker count).
    template <task_pool_like Pool>
    std::vector<std::vector<std::uint32_t>> encode_batch(std::span<const std::string_view> docs, Pool& pool,
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

    /// \brief Forgets the calling thread's merged-piece cache, so the next encode on this thread starts cold.
    static void clear_thread_cache() noexcept
    {
        thread_cache().owner = 0;
#if TOK_MEMO
        thread_memo().owner = 0;
#endif
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
    // benchmark corpus). Two-way set associative, 128-byte entries, 32768 of them: 4 MB per thread, the budget
    // toktok's memo uses. Measured on the 12.3 MB corpus (i7-7700K, cl100k, best of 5): against the 2048-entry
    // direct-mapped cache this replaced, one thread 76 -> 81 MB/s and 8 threads 397 -> 455 MB/s, the gain
    // being repeated pieces; a cold first pass is unchanged (76 MB/s), since a cache cannot help text it has
    // not seen. 65536 entries (8 MB) gave 86 / 465. Thread-local, so it is never shared and needs no locking;
    // it is tagged with the encoder that filled it, so a thread that moves to another vocabulary starts clean.
    struct piece_cache {
        struct entry {
            std::uint64_t hash;
            std::uint8_t len, count;
            char bytes[46];
            std::uint32_t tokens[18];
        };
        static_assert(sizeof(entry) == 128);
#ifndef TOK_PIECE_CACHE_SIZE
#define TOK_PIECE_CACHE_SIZE 32768
#endif
#ifndef TOK_PIECE_CACHE_WAYS
#define TOK_PIECE_CACHE_WAYS 2
#endif
        static constexpr std::size_t size = TOK_PIECE_CACHE_SIZE;   // entries, a power of two
        static constexpr std::size_t ways = TOK_PIECE_CACHE_WAYS;   // entries per set: 1 or 2
        static_assert((size & (size - 1)) == 0 && (ways == 1 || ways == 2) && size >= ways);
        static constexpr std::size_t sets = size / ways;

        std::uint64_t owner = 0;
        std::unique_ptr<entry[]> entries{new entry[size]};

        void reset(std::uint64_t id)
        {
            owner = id;
            for (std::size_t i = 0; i < size; ++i)
                entries[i].len = 0;   // len 0 is empty: a cached piece has at least two bytes
        }
        static std::uint64_t hash(std::string_view piece) noexcept { return wyhash(piece); }
        static bool holds(const entry& e, std::uint64_t h, std::string_view piece) noexcept
        {
            return e.hash == h && e.len == piece.size() && std::memcmp(e.bytes, piece.data(), piece.size()) == 0;
        }
        // Two ways: a set is two adjacent entries; a hit in the second promotes it to the first, and an insert
        // goes to the first and moves the first to the second, so the set keeps its two most recent pieces
        const entry* find(std::uint64_t h, std::string_view piece) noexcept
        {
            entry* set = &entries[(h & (sets - 1)) * ways];
            if (holds(set[0], h, piece))
                return &set[0];
            if constexpr (ways == 2) {
                if (holds(set[1], h, piece)) {
                    std::swap(set[0], set[1]);
                    return &set[0];
                }
            }
            return nullptr;
        }
        void insert(std::uint64_t h, std::string_view piece, const std::uint32_t* tokens, std::size_t count) noexcept
        {
            if (piece.size() > sizeof(entry::bytes) || count > std::size(entry{}.tokens))
                return;
            entry* set = &entries[(h & (sets - 1)) * ways];
            if constexpr (ways == 2)
                set[1] = set[0];
            entry& e = set[0];
            e.hash = h;
            e.len = static_cast<std::uint8_t>(piece.size());
            e.count = static_cast<std::uint8_t>(count);
            std::memcpy(e.bytes, piece.data(), piece.size());
            std::memcpy(e.tokens, tokens, count * sizeof(std::uint32_t));
        }
    };
    // The memo of short pieces, per thread: every piece of up to 15 bytes this thread has encoded, with its
    // tokens, so a piece seen before costs one probe of a table that sits in L2/L3 instead of a vocabulary
    // lookup (and, for the few that are not one token, a merge-cache lookup). The idea is toktok's.
    //
    // The key is the piece's bytes as two overlapping 8-byte words and its length, built without reading past
    // the piece (as vocab.hpp's head word is), so it is exact and clean under AddressSanitizer: for n <= 8 the
    // first word holds every byte and the second is 0; for 9..15 the second is the last eight bytes, and with
    // the length the pair determines the piece. Entries are 32 bytes, two to a 64-byte line, and a set is the
    // line: a hit in the second way is moved to the first, an insert goes to the first. Pieces of more than 3
    // tokens are not remembered (a piece of up to 15 bytes rarely has more).
    struct piece_memo {
        static constexpr std::size_t max_bytes = 15, max_tokens = 3;
        struct key {
            std::uint64_t lo, hi;
            std::uint32_t len;
        };
        struct entry {
            std::uint64_t lo, hi;
            std::uint8_t len, count;   // len 0xff: empty (no piece is that long here)
            std::uint16_t pad;
            std::uint32_t tokens[max_tokens];
            bool same(const key& k) const noexcept { return lo == k.lo && hi == k.hi && len == k.len; }
        };
        static_assert(sizeof(entry) == 32);
#ifndef TOK_MEMO_SETS
#define TOK_MEMO_SETS 65536
#endif
        static constexpr std::size_t sets = TOK_MEMO_SETS;   // two entries each: 4 MB at 65536
        static_assert((sets & (sets - 1)) == 0);

        std::uint64_t owner = 0;
        struct alignas(64) line {   // a set: two ways in one cache line (over-aligned new and delete[] pair up)
            entry way[2];
        };
        std::unique_ptr<line[]> lines{new line[sets]};

        void reset(std::uint64_t id)
        {
            owner = id;
            for (std::size_t i = 0; i < sets; ++i)
                lines[i].way[0].len = lines[i].way[1].len = 0xff;
        }
        static key key_of(std::string_view piece) noexcept
        {
            const auto* p = reinterpret_cast<const unsigned char*>(piece.data());
            const std::size_t n = piece.size();
            return {detail::head8(p, n), n > 8 ? detail::load8(p + n - 8) : 0, static_cast<std::uint32_t>(n)};
        }
        // A key's set: two entries in one 64-byte line
        entry* set_of(const key& k) noexcept
        {
            const std::uint64_t h = wymix(k.lo ^ 0x2d358dccaa6c78a5ull, k.hi ^ (k.len * 0x8bb84b93962eacc9ull));
            return lines[h & (sets - 1)].way;
        }
        // The set's first way, holding k if the set does (a hit in the second way is moved to the first)
        static entry& way(entry* set, const key& k) noexcept
        {
            if (!set[0].same(k) && set[1].same(k))
                std::swap(set[0], set[1]);
            return set[0];
        }
        void remember(entry& e, const key& k, const std::uint32_t* tokens, std::size_t count) noexcept
        {
            if (count > max_tokens)
                return;
            (&e)[1] = e;   // the first way moves to the second
            e.lo = k.lo;
            e.hi = k.hi;
            e.len = static_cast<std::uint8_t>(k.len);
            e.count = static_cast<std::uint8_t>(count);
            std::memcpy(e.tokens, tokens, count * sizeof(std::uint32_t));
        }
    };
    static piece_memo& thread_memo()
    {
        thread_local piece_memo memo;
        return memo;
    }

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

    // The tokens of every one- and two-byte piece, worked out once from the vocabulary: a byte is one token in
    // a byte-level vocabulary; two bytes are one token if the pair is in the vocabulary and otherwise the two
    // bytes' tokens, since the pair itself is the only merge there is. 1 KB and 512 KB, so the hot entries live
    // in L1 and L2, where the memo (a few MB) cannot. Left empty for a vocabulary that is not byte-level, which
    // then takes the general path and its error.
    struct direct2 {
        std::uint32_t first, second;   // second is no_rank for a pair that is one token
    };
    void build_direct()
    {
        for (unsigned b = 0; b < 256; ++b) {
            const char c = static_cast<char>(b);
            one_byte_[b] = vocab_(std::string_view(&c, 1));
            if (one_byte_[b] == no_rank)
                return;
        }
        two_byte_.resize(65536);
        for (unsigned v = 0; v < 65536; ++v) {
            const char pair[2] = {static_cast<char>(v & 0xff), static_cast<char>(v >> 8)};
            const std::uint32_t whole = vocab_(std::string_view(pair, 2));
            two_byte_[v] = whole != no_rank ? direct2{whole, no_rank} : direct2{one_byte_[v & 0xff], one_byte_[v >> 8]};
        }
    }

    tokenizer::vocab vocab_;
    split_pattern pattern_;
    std::array<std::uint32_t, 256> one_byte_{};
    std::vector<direct2> two_byte_;
    std::uint64_t id_ = next_id();
};

} // namespace snicholls::tokenizer

#endif // ts_moveables_tokenizer_encoder_hpp
