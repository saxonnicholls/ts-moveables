//
//  long_merge.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  The byte-pair merge in O(n log n), for long pieces. A candidate for snicholls-core's ml/bpe.hpp.
//
//  bpe_encode_piece is tiktoken's byte_pair_merge: after each merge it rescans every part for the lowest
//  rank, so a piece of n bytes costs O(n^2). Pieces are short in real text and that loop is the fastest
//  there is for them, but one long piece (a run of a megabyte of spaces, a minified line, a base64 blob that
//  the split pattern keeps whole) is quadratic, in tiktoken as here.
//
//  This is the same merge with the rescan replaced by a binary heap of candidate pairs keyed (rank, start).
//  The heap's minimum is the lowest rank, and among equal ranks the leftmost start, which is exactly the pair
//  tiktoken merges, so the tokens are the same by construction (and are tested to be on every piece of the
//  corpus). Merging changes the pair to the left and the pair starting here; their new ranks are pushed and
//  the old entries are left in the heap and skipped when they surface. An entry is current exactly when its
//  part is still alive and its rank is the part's current pair rank: two different byte strings never share
//  a rank, so an equal rank means the same pair.
//
//  Alloc-free and constexpr, like bpe_encode_piece: everything lives in the caller's scratch, at most
//  long_merge_scratch_size(n) parts.
//

#ifndef ts_moveables_tokenizer_long_merge_hpp
#define ts_moveables_tokenizer_long_merge_hpp

#include <snicholls/core/ml/bpe.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace snicholls::tokenizer
{

/// \brief The scratch parts long_merge needs for a piece of `n` bytes: the links (2n) and the heap (3n).
constexpr std::size_t long_merge_scratch_size(std::size_t n) noexcept { return 5 * n + 1; }

/// \brief Encodes one piece as ml::bpe_encode_piece does, in O(n log n) rank lookups and heap steps.
/// `out` needs piece.size() slots, `scratch` long_merge_scratch_size(piece.size()) parts.
template <ml::bpe_vocabulary Rank>
constexpr result<std::size_t, ml::bpe_error> long_merge(std::string_view piece, Rank&& rank, std::span<std::uint32_t> out,
                                                        std::span<ml::bpe_part> scratch)
{
    using ml::bpe_no_rank;
    using ml::bpe_part;
    const std::size_t n = piece.size();
    if (n == 0)
        return ok(std::size_t{0});
    if (out.empty())
        return err(ml::bpe_error::output_too_small);
    if (const std::uint32_t whole = rank(piece); whole != bpe_no_rank) {   // as bpe_encode_piece and tiktoken
        out[0] = whole;
        return ok(std::size_t{1});
    }
    if (out.size() < n)
        return err(ml::bpe_error::output_too_small);
    if (scratch.size() < long_merge_scratch_size(n))
        return err(ml::bpe_error::scratch_too_small);

    // link[i] = {start of the next part, rank of part i merged with it} for a live part starting at i;
    // back[i] = {start of the previous part (n for none), 1 while alive}. Starts index the piece's bytes.
    bpe_part* link = scratch.data();
    bpe_part* back = link + n;
    bpe_part* heap = back + n;   // {start, rank}, ordered by (rank, start)
    std::size_t size = 0;
    const auto less = [](const bpe_part& a, const bpe_part& b) {
        return a.rank < b.rank || (a.rank == b.rank && a.start < b.start);
    };
    const auto push = [&](bpe_part e) {
        std::size_t i = size++;
        while (i > 0) {
            const std::size_t p = (i - 1) / 2;
            if (!less(e, heap[p]))
                break;
            heap[i] = heap[p];
            i = p;
        }
        heap[i] = e;
    };
    const auto pop = [&] {
        const bpe_part top = heap[0];
        const bpe_part e = heap[--size];
        std::size_t i = 0;
        for (;;) {
            std::size_t c = 2 * i + 1;
            if (c >= size)
                break;
            if (c + 1 < size && less(heap[c + 1], heap[c]))
                ++c;
            if (!less(heap[c], e))
                break;
            heap[i] = heap[c];
            i = c;
        }
        if (size > 0)
            heap[i] = e;
        return top;
    };
    // The rank of the part at i merged with its next part: the bytes from i to the end of the next part
    const auto pair_rank = [&](std::uint32_t i) -> std::uint32_t {
        const std::uint32_t j = link[i].start;
        if (j >= n)
            return bpe_no_rank;
        const std::uint32_t end = link[j].start;
        return rank(piece.substr(i, end - i));
    };

    for (std::uint32_t i = 0; i < n; ++i) {
        link[i] = {i + 1, bpe_no_rank};
        back[i] = {i == 0 ? static_cast<std::uint32_t>(n) : i - 1, 1};
    }
    for (std::uint32_t i = 0; i + 1 < n; ++i) {
        link[i].rank = rank(piece.substr(i, 2));
        if (link[i].rank != bpe_no_rank)
            push({i, link[i].rank});
    }

    while (size > 0) {
        const bpe_part e = pop();
        const std::uint32_t i = e.start;
        if (back[i].rank == 0 || link[i].rank != e.rank)   // merged away, or its pair has changed since
            continue;
        // Merge part i with the next part j: j leaves the list
        const std::uint32_t j = link[i].start;
        const std::uint32_t k = link[j].start;
        back[j].rank = 0;
        link[i].start = k;
        if (k < n)
            back[k].start = i;
        if ((link[i].rank = pair_rank(i)) != bpe_no_rank)
            push({i, link[i].rank});
        if (const std::uint32_t p = back[i].start; p < n)
            if ((link[p].rank = pair_rank(p)) != bpe_no_rank)
                push({p, link[p].rank});
    }

    std::size_t count = 0;
    for (std::uint32_t i = 0; i < n; i = link[i].start) {
        out[count] = rank(piece.substr(i, link[i].start - i));
        if (out[count] == bpe_no_rank)
            return err(ml::bpe_error::unknown_bytes);
        ++count;
    }
    return ok(count);
}

} // namespace snicholls::tokenizer

#endif // ts_moveables_tokenizer_long_merge_hpp
