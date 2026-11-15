//
//  suffix_array.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - a parallel suffix array by prefix doubling
//  (Manber-Myers), shared by the BWT demo and the persistent index demo
//
//  Each round sorts the suffixes by a pair of ranks and renumbers them, and
//  those two steps are exactly parallel_sort and a prefix scan:
//
//      sort      parallel_sort of the suffix indices by (rank[i], rank[i+k])
//      re-rank   a flag per adjacent pair saying "these differ"; the INCLUSIVE
//                SCAN of the flags IS the new rank
//      scatter   parallel_for writes the new ranks back by suffix
//
//  log2(n) rounds of that. It is not the fastest suffix array algorithm (the
//  divsufsort family would win); it is assembled only from this library's
//  parallel primitives, with an honest number attached.
//
//  C++17, like the BWT demo that shares it: arrays are passed as pointers and
//  a length rather than as std::span.
//
//  The rounds are exposed one at a time, over the caller's arrays, so a caller
//  can keep the arrays anywhere (a std::vector, or a memory-mapped file) and
//  checkpoint between rounds. A round READS `rank` and WRITES `next_rank`, so
//  the state before the round stays intact until the caller swaps them: a
//  build killed mid-round resumes from the last completed round. `sa` needs no
//  checkpoint, because a round only needs it to hold some permutation: the
//  sort fully determines the order from the ranks. A resumed build resets it
//  to the identity.
//
//  Also here, so neither demo carries its own: the synthetic DNA generator.
//

#ifndef ts_moveables_demos_text_index_suffix_array_hpp
#define ts_moveables_demos_text_index_suffix_array_hpp

#include "../../TSMoveables/concurrent/parallel_for.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace snicholls::text_index {

// Round zero: sa is the identity, and a suffix's rank is its first byte
template <TS_MOVEABLES_TASK_POOL Pool>
void suffix_array_init(Pool& pool, std::string_view s, std::int32_t* sa, std::int32_t* rank)
{
    parallel_for(pool, std::size_t{0}, s.size(), [&](std::size_t i) {
        sa[i] = static_cast<std::int32_t>(i);
        rank[i] = static_cast<unsigned char>(s[i]);
    });
}

// Put sa back to the identity, before a round resumed from a checkpoint
template <TS_MOVEABLES_TASK_POOL Pool>
void suffix_array_reset_order(Pool& pool, std::int32_t* sa, std::size_t n)
{
    parallel_for(pool, std::size_t{0}, n, [&](std::size_t i) { sa[i] = static_cast<std::int32_t>(i); });
}

// One doubling round at offset k: sorts sa by (rank[i], rank[i+k]) and writes
// the new ranks into next_rank. Returns true once every suffix has a rank of
// its own: sa is then the suffix array. `flags` and `scanned` are workspace.
template <TS_MOVEABLES_TASK_POOL Pool>
bool suffix_array_round(Pool& pool, std::size_t n, std::size_t k, std::int32_t* sa, const std::int32_t* rank,
                        std::int32_t* next_rank, std::int32_t* flags, std::int32_t* scanned)
{
    // A missing second half sorts first: the sentinel's job, made explicit
    const auto key2 = [&](std::int32_t i) -> std::int32_t {
        const std::size_t j = static_cast<std::size_t>(i) + k;
        return j < n ? rank[j] : -1;
    };
    const auto less = [&](std::int32_t a, std::int32_t b) {
        if (rank[static_cast<std::size_t>(a)] != rank[static_cast<std::size_t>(b)])
            return rank[static_cast<std::size_t>(a)] < rank[static_cast<std::size_t>(b)];
        return key2(a) < key2(b);
    };

    parallel_sort(pool, sa, sa + n, less);

    // "Does this suffix differ from the one before it?" - one flag each
    flags[0] = 0;
    parallel_for(pool, std::size_t{1}, n, [&](std::size_t i) { flags[i] = less(sa[i - 1], sa[i]) ? 1 : 0; });

    // The prefix sum of those flags IS the new rank, in sorted order
    parallel_inclusive_scan(pool, flags, flags + n, scanned, std::int32_t{0},
                            std::plus<std::int32_t>{});

    parallel_for(pool, std::size_t{0}, n,
                 [&](std::size_t i) { next_rank[static_cast<std::size_t>(sa[i])] = scanned[i]; });
    return static_cast<std::size_t>(scanned[n - 1]) == n - 1;   // every suffix has its own rank
}

// The whole build, in memory. `s` carries its sentinel.
template <TS_MOVEABLES_TASK_POOL Pool>
std::vector<std::int32_t> suffix_array(Pool& pool, std::string_view s)
{
    const std::size_t n = s.size();
    std::vector<std::int32_t> sa(n), rank(n), next(n), flags(n), scanned(n);
    suffix_array_init(pool, s, sa.data(), rank.data());
    for (std::size_t k = 1;; k *= 2) {
        const bool done = suffix_array_round(pool, n, k, sa.data(), rank.data(), next.data(), flags.data(), scanned.data());
        rank.swap(next);
        if (done)
            break;
    }
    return sa;
}

// Synthetic DNA with local structure: mostly a random walk over ACGT, with a
// repeated motif spliced in, as real genomes have, which is what makes BWT's
// clustering visible and gives a search index repeated hits. Streams, so a
// text larger than memory can be generated straight into a file.
class dna_generator
{
public:
    explicit dna_generator(unsigned seed) : rng_(seed) {}

    // The next `n` bases into `out`
    void fill(char* out, std::size_t n)
    {
        static const char bases[] = "ACGT";
        static const std::string_view motif = "ACGTACGTTTTACGT";
        std::size_t at = 0;
        while (at < n) {
            if (pending_ < motif.size() && pending_motif_) {
                out[at++] = motif[pending_++];
                continue;
            }
            pending_motif_ = false;
            if (run_left_ > 0) {
                out[at++] = bases[rng_() % 4];
                --run_left_;
                continue;
            }
            if (rng_() % 5 == 0) {
                pending_motif_ = true;   // a repeat, the thing BWT clusters
                pending_ = 0;
            } else {
                run_left_ = 1 + rng_() % 12;
            }
        }
    }

private:
    std::mt19937 rng_;
    std::size_t pending_ = 0;
    bool pending_motif_ = false;
    std::size_t run_left_ = 0;
};

inline std::string make_dna(std::size_t n, unsigned seed)
{
    std::string s(n, '\0');
    dna_generator g(seed);
    g.fill(s.data(), n);
    return s;
}

} // namespace snicholls::text_index

#endif /* ts_moveables_demos_text_index_suffix_array_hpp */
