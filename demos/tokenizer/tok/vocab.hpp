//
//  vocab.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  A byte-level BPE vocabulary: bytes to rank, and rank to bytes. It is built once and then read from every
//  thread at the same time, so it is immutable after construction and needs no synchronisation.
//
//  The lookup is the inner loop of the merge, so it is laid out for it. Open addressing in one flat array of
//  16-byte slots, linear probing, at most half full. Each slot holds the first eight bytes of its token as a
//  word ("head"), so for the tokens of eight bytes or fewer, which are most of them, the comparison is one
//  word and the length, and the slot is the only memory touched. Longer tokens compare the rest against an
//  arena. The head is built from the piece without reading past its end (overlapping loads, as wyhash does),
//  so the lookup is clean under AddressSanitizer.
//
//  rank(bytes) is `bpe_no_rank` for bytes that are not a token, as snicholls::ml::bpe_encode_piece expects.
//

#ifndef ts_moveables_tokenizer_vocab_hpp
#define ts_moveables_tokenizer_vocab_hpp

#include <snicholls/core/hash/wyhash.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace snicholls::tokenizer
{

inline constexpr std::uint32_t no_rank = 0xffffffffu;

namespace detail
{

inline std::uint64_t load8(const unsigned char* p) noexcept
{
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}
inline std::uint64_t load4(const unsigned char* p) noexcept
{
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

// The first min(n, 8) bytes as a little-endian word, zero above them, reading only [p, p + n)
inline std::uint64_t head8(const unsigned char* p, std::size_t n) noexcept
{
    if (n >= 8)
        return load8(p);
    if (n >= 4)   // the two loads overlap on equal bytes, so OR is exact
        return load4(p) | (load4(p + n - 4) << (8 * (n - 4)));
    if (n == 0)
        return 0;
    return std::uint64_t{p[0]} | (std::uint64_t{p[n >> 1]} << (8 * (n >> 1))) | (std::uint64_t{p[n - 1]} << (8 * (n - 1)));
}

inline std::uint64_t key_hash(const unsigned char* p, std::size_t n, std::uint64_t head) noexcept
{
    if (n <= 8)
        return wymix(head ^ 0xa0761d6478bd642full, n ^ 0xe7037ed1a0b428dbull);
    if (n <= 16)
        return wymix(head ^ 0x8ebc6af09c88c6e3ull, load8(p + n - 8) ^ (n * 0x589965cc75374cc3ull));
    return wyhash(std::string_view(reinterpret_cast<const char*>(p), n));
}

} // namespace detail

class vocab
{
public:
    vocab() = default;

    /// \brief Builds from (bytes, rank) pairs. Ranks must be distinct; bytes must be at most 255 long.
    explicit vocab(const std::vector<std::pair<std::string, std::uint32_t>>& tokens)
    {
        std::size_t cap = 16;
        while (cap < tokens.size() * 2)
            cap <<= 1;
        slots_.assign(cap, slot{0, no_rank, 0});
        mask_ = cap - 1;
        std::uint32_t max_rank = 0;
        for (const auto& t : tokens)
            max_rank = t.second > max_rank ? t.second : max_rank;
        by_rank_.assign(tokens.empty() ? 0 : std::size_t{max_rank} + 1, {0, 0});
        for (const auto& [bytes, r] : tokens)
            arena_ += bytes;
        if (arena_.size() >= (1u << 24))
            throw std::length_error("vocab: more than 16 MB of token bytes");
        std::uint32_t off = 0;
        for (const auto& [bytes, r] : tokens) {
            if (bytes.empty() || bytes.size() > 255)
                throw std::length_error("vocab: a token of 0 or more than 255 bytes");
            if (r == no_rank)
                throw std::invalid_argument("vocab: rank 0xffffffff is reserved");
            const auto* p = reinterpret_cast<const unsigned char*>(arena_.data()) + off;
            const std::uint64_t head = detail::head8(p, bytes.size());
            std::size_t i = detail::key_hash(p, bytes.size(), head) & mask_;
            while (slots_[i].rank != no_rank) {
                if (slots_[i].rank == r || bytes == token(slots_[i].rank))
                    throw std::invalid_argument("vocab: a duplicate token or rank");
                i = (i + 1) & mask_;
            }
            slots_[i] = slot{head, r, (off << 8) | static_cast<std::uint32_t>(bytes.size())};
            by_rank_[r] = {off, static_cast<std::uint32_t>(bytes.size())};
            off += static_cast<std::uint32_t>(bytes.size());
            ++size_;
        }
    }

    /// \brief A lookup split in two, so that many can be in flight: `probe` hashes and prefetches the slot,
    /// `find` finishes it. Encoding probes a batch of pieces and then finds them, so the cache misses on a
    /// table larger than L2 overlap instead of queueing one behind the other.
    struct probe_t {
        std::uint64_t head, hash;
    };
    probe_t probe(std::string_view bytes) const noexcept
    {
        const auto* p = reinterpret_cast<const unsigned char*>(bytes.data());
        const std::uint64_t head = detail::head8(p, bytes.size());
        const probe_t r{head, detail::key_hash(p, bytes.size(), head)};
#if defined(__GNUC__) || defined(__clang__)
        __builtin_prefetch(&slots_[r.hash & mask_]);
#endif
        return r;
    }
    std::uint32_t find(std::string_view bytes, probe_t pr) const noexcept
    {
        const auto* p = reinterpret_cast<const unsigned char*>(bytes.data());
        const std::size_t n = bytes.size();
        const std::uint64_t head = pr.head;
        for (std::size_t i = pr.hash & mask_;; i = (i + 1) & mask_) {
            const slot& e = slots_[i];
            if (e.rank == no_rank)
                return no_rank;
            if (e.head == head && (e.off_len & 0xffu) == n &&
                (n <= 8 || std::memcmp(arena_.data() + (e.off_len >> 8) + 8, p + 8, n - 8) == 0))
                return e.rank;
        }
    }

    /// \brief The rank of `bytes`, or `no_rank`.
    std::uint32_t operator()(std::string_view bytes) const noexcept
    {
        const auto* p = reinterpret_cast<const unsigned char*>(bytes.data());
        const std::size_t n = bytes.size();
        const std::uint64_t head = detail::head8(p, n);
        for (std::size_t i = detail::key_hash(p, n, head) & mask_;; i = (i + 1) & mask_) {
            const slot& e = slots_[i];
            if (e.rank == no_rank)
                return no_rank;
            if (e.head == head && (e.off_len & 0xffu) == n &&
                (n <= 8 || std::memcmp(arena_.data() + (e.off_len >> 8) + 8, p + 8, n - 8) == 0))
                return e.rank;
        }
    }

    /// \brief The bytes of token `rank`; empty if it is not one.
    std::string_view token(std::uint32_t rank) const noexcept
    {
        if (rank >= by_rank_.size())
            return {};
        return std::string_view(arena_).substr(by_rank_[rank].first, by_rank_[rank].second);
    }

    std::size_t size() const noexcept { return size_; }
    std::size_t memory_bytes() const noexcept { return slots_.size() * sizeof(slot) + arena_.size() + by_rank_.size() * 8; }

private:
    struct slot {
        std::uint64_t head;
        std::uint32_t rank;
        std::uint32_t off_len;   // arena offset << 8 | length
    };
    static_assert(sizeof(slot) == 16);

    std::vector<slot> slots_;
    std::size_t mask_ = 0;
    std::size_t size_ = 0;
    std::string arena_;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> by_rank_;   // offset, length
};

} // namespace snicholls::tokenizer

#endif // ts_moveables_tokenizer_vocab_hpp
