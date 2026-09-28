//
//  bwt_dna_demo.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - the Burrows-Wheeler transform, over DNA
//
//  A demo rather than a component, and deliberately so. A compression
//  transform has no business inside a concurrency library, but it is an
//  unusually good way to show that the parallel algorithms compose into
//  something real: everything below is built from parallel_for, parallel_sort
//  and parallel_inclusive_scan and nothing else. No dependencies - not even
//  the author's own base-encode-decode, whose 2-bit ACGT packing inspired the
//  alphabet here but is not linked; this file generates and handles its own
//  nucleotides so the demo stays self-contained.
//
//  ---------------------------------------------------------------- the idea
//
//  BWT sorts every rotation of a string and keeps the last column. The
//  celebrated part is that this is reversible, and that the output CLUSTERS:
//  characters sharing a right-context end up adjacent. That is why bzip2 runs
//  BWT before its entropy coder, and it is why DNA is such a good subject -
//  a four-letter alphabet with heavy local structure produces long runs.
//
//  The transform is computed here from a SUFFIX ARRAY, which is the standard
//  route: append a sentinel smaller than every real character and the sorted
//  suffixes are in the same order as the sorted rotations, so
//
//      BWT[i] = s[(SA[i] + n - 1) mod n]
//
//  ------------------------------------------------ why this exercises the library
//
//  The suffix array is built by prefix doubling (Manber-Myers). Each round
//  sorts the suffixes by a pair of ranks, then renumbers them - and those two
//  steps are exactly parallel_sort and a prefix scan:
//
//      sort      parallel_sort over the suffix indices by (rank[i], rank[i+k])
//      re-rank   a flag per adjacent pair saying "these differ", then an
//                INCLUSIVE SCAN of those flags IS the new rank - a prefix sum
//                is the whole renumbering step, not an incidental use of one
//      scatter   parallel_for to write the ranks back
//
//  log2(n) rounds of that. It is not the fastest suffix array algorithm in
//  existence and makes no claim to be - libdivsufsort and parallel-divsufsort
//  are specialised, heavily tuned, and would win. The point here is
//  composition: a real algorithm assembled only from this library's parallel
//  primitives, with an honest number attached.
//
//  The inverse transform is deliberately left SERIAL. It is a pointer chase
//  through the LF mapping, where each step depends on the previous one - the
//  shape that has no parallel form worth writing - and the character counts it
//  needs are 4 to 256 entries, far under the ~128-element crossover the
//  benchmark measured for a parallel call to be worth making at all. Running
//  it serially is the correct answer, not a shortcut.
//

#include "../TSMoveables/concurrent/parallel_for.hpp"
#include "../TSMoveables/concurrent/thread_pool.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <vector>

using namespace snicholls;
using clk = std::chrono::steady_clock;

namespace {

constexpr char sentinel = '\x01';           // below 'A', so it sorts first

double ms_since(clk::time_point t0)
{
    return std::chrono::duration<double, std::milli>(clk::now() - t0).count();
}

// ------------------------------------------------------------- suffix array
//
// Prefix doubling. `s` already carries its sentinel.
std::vector<std::int32_t> suffix_array(task_pool& pool, const std::string& s)
{
    const std::size_t n = s.size();
    std::vector<std::int32_t> sa(n), rank(n), tmp(n);
    std::vector<std::int32_t> flags(n), scanned(n);

    parallel_for(pool, std::size_t{0}, n, [&](std::size_t i) {
        sa[i]   = static_cast<std::int32_t>(i);
        rank[i] = static_cast<unsigned char>(s[i]);
    });

    for (std::size_t k = 1;; k *= 2) {
        // Order by (rank[i], rank[i+k]), with a missing second half sorting
        // first - the sentinel's job, made explicit.
        const auto key2 = [&](std::int32_t i) -> std::int32_t {
            const std::size_t j = static_cast<std::size_t>(i) + k;
            return j < n ? rank[j] : -1;
        };
        const auto less = [&](std::int32_t a, std::int32_t b) {
            if (rank[a] != rank[b])
                return rank[a] < rank[b];
            return key2(a) < key2(b);
        };

        parallel_sort(pool, sa.begin(), sa.end(), less);

        // "Does this suffix differ from the one before it?" - one flag each.
        flags[0] = 0;
        parallel_for(pool, std::size_t{1}, n, [&](std::size_t i) {
            flags[i] = less(sa[i - 1], sa[i]) ? 1 : 0;
        });

        // The prefix sum of those flags IS the new rank, in sorted order.
        parallel_inclusive_scan(pool, flags.begin(), flags.end(), scanned.begin(),
                                std::int32_t{0}, std::plus<std::int32_t>{});

        parallel_for(pool, std::size_t{0}, n, [&](std::size_t i) {
            tmp[static_cast<std::size_t>(sa[i])] = scanned[i];
        });
        rank.swap(tmp);

        if (static_cast<std::size_t>(rank[static_cast<std::size_t>(sa[n - 1])]) == n - 1)
            break;                          // every suffix has its own rank
    }
    return sa;
}

std::string bwt_forward(task_pool& pool, const std::string& with_sentinel)
{
    const std::size_t n = with_sentinel.size();
    const std::vector<std::int32_t> sa = suffix_array(pool, with_sentinel);

    std::string out(n, '\0');
    parallel_for(pool, std::size_t{0}, n, [&](std::size_t i) {
        const std::size_t p = static_cast<std::size_t>(sa[i]);
        out[i] = with_sentinel[(p + n - 1) % n];
    });
    return out;
}

// Serial on purpose - see the header. LF mapping, walked backwards.
std::string bwt_inverse(const std::string& bwt)
{
    const std::size_t n = bwt.size();
    std::array<std::size_t, 256> count{};
    for (const char c : bwt)
        ++count[static_cast<unsigned char>(c)];

    std::array<std::size_t, 256> first{};   // where each character's block starts
    std::size_t running = 0;
    for (std::size_t c = 0; c < 256; ++c) {
        first[c] = running;
        running += count[c];
    }

    // rank_in_column[i] = how many copies of bwt[i] appear before position i
    std::vector<std::size_t> lf(n);
    std::array<std::size_t, 256> seen{};
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(bwt[i]);
        lf[i] = first[c] + seen[c]++;
    }

    std::string out(n, '\0');
    std::size_t p = 0;                      // row 0 is the sentinel's rotation
    for (std::size_t i = n; i-- > 0;) {
        out[i] = bwt[p];
        p = lf[p];
    }

    // The walk reconstructs the ROW it started from, and row 0 is the rotation
    // that begins with the sentinel - so this is "$" + original, one rotation
    // away from what the caller handed in. Rotating the sentinel to the back
    // returns the original string with its sentinel where it was.
    return out.substr(1) + out.front();
}

// Longest run and mean run length - the property BWT exists to create.
std::pair<std::size_t, double> run_stats(const std::string& s)
{
    if (s.empty())
        return {0, 0.0};
    std::size_t runs = 1, longest = 1, current = 1;
    for (std::size_t i = 1; i < s.size(); ++i) {
        if (s[i] == s[i - 1]) {
            ++current;
        } else {
            ++runs;
            longest = std::max(longest, current);
            current = 1;
        }
    }
    longest = std::max(longest, current);
    return {longest, double(s.size()) / double(runs)};
}

// Synthetic DNA with local structure: mostly a random walk over ACGT, but with
// repeated motifs spliced in, which is what real genomes look like and what
// makes the transform's clustering visible.
std::string make_dna(std::size_t n, unsigned seed)
{
    static const char bases[] = "ACGT";
    std::mt19937 rng(seed);
    std::string s;
    s.reserve(n);
    const std::string motif = "ACGTACGTTTTACGT";
    while (s.size() < n) {
        if (rng() % 5 == 0) {
            s += motif;                     // a repeat, the thing BWT clusters
        } else {
            const std::size_t run = 1 + rng() % 12;
            for (std::size_t i = 0; i < run && s.size() < n; ++i)
                s.push_back(bases[rng() % 4]);
        }
    }
    s.resize(n);
    return s;
}

void check(bool ok, const char* what)
{
    if (!ok) {
        std::printf("  FAILED: %s\n", what);
        std::exit(1);
    }
}

} // namespace

int main(int argc, char** argv)
{
    bool quick = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--quick")
            quick = true;

    work_stealing_task_pool pool;
    std::printf("Burrows-Wheeler over DNA - parallel_sort + parallel_scan, %zu workers\n\n",
                pool.worker_count());

    // ------------------------------------------------ a worked example
    {
        const std::string dna = "ACGTACGTACGT";
        const std::string t   = bwt_forward(pool, dna + sentinel);
        std::string shown = t;
        std::replace(shown.begin(), shown.end(), sentinel, '$');

        std::printf("  a worked example\n");
        std::printf("    input                    %s\n", dna.c_str());
        std::printf("    BWT ($ = sentinel)       %s\n", shown.c_str());

        const std::string back = bwt_inverse(t);
        check(back == dna + sentinel, "worked example round-trip");
        std::printf("    inverse                  %s  (round-trip exact)\n\n", dna.c_str());
    }

    // ------------------------------------------------ round-trip at size
    {
        const std::size_t n = quick ? 50000 : 400000;
        const std::string dna = make_dna(n, 20260929u);
        const auto t0 = clk::now();
        const std::string t = bwt_forward(pool, dna + sentinel);
        const double fwd = ms_since(t0);

        const auto t1 = clk::now();
        const std::string back = bwt_inverse(t);
        const double inv = ms_since(t1);
        check(back == dna + sentinel, "large round-trip");

        const auto before = run_stats(dna);
        const auto after  = run_stats(t);

        std::printf("  %zu bases\n", n);
        std::printf("    forward (parallel)       %8.1f ms\n", fwd);
        std::printf("    inverse (serial)         %8.1f ms\n", inv);
        std::printf("    round-trip               exact\n");
        std::printf("    longest run  before %5zu  ->  after %5zu\n", before.first, after.first);
        std::printf("    mean run     before %5.2f  ->  after %5.2f   (x%.1f)\n\n",
                    before.second, after.second, after.second / before.second);
    }

    // ------------------------------------------------ serial vs parallel
    {
        const std::size_t n = quick ? 50000 : 400000;
        const std::string dna = make_dna(n, 7u) + sentinel;

        mutex_task_pool one(1);             // worker_count()==1 takes every serial path
        const auto t0 = clk::now();
        const std::string a = bwt_forward(one, dna);
        const double serial = ms_since(t0);

        const auto t1 = clk::now();
        const std::string b = bwt_forward(pool, dna);
        const double par = ms_since(t1);

        check(a == b, "serial and parallel agree");

        std::printf("  serial vs parallel (same code path, one worker vs %zu)\n", pool.worker_count());
        std::printf("    serial                   %8.1f ms\n", serial);
        std::printf("    parallel                 %8.1f ms   %.2fx\n", par, serial / par);
        std::printf("    identical output         yes\n\n");
        std::printf("    log2(n) rounds, each one a parallel_sort plus a scan and a\n");
        std::printf("    synchronisation - so this tracks parallel_sort's ceiling rather\n");
        std::printf("    than parallel_reduce's, and that is the honest expectation.\n\n");
    }

    std::printf("all BWT/DNA demo checks passed\n");
    return 0;
}
