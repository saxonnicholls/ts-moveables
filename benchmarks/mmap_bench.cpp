//
//  mmap_bench.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  mapped_vector against std::vector, like for like (`make bench-mmap`):
//
//    push_back   N ticks appended one at a time, no reserve, from empty: the
//                file-backed vector grows its file (preallocated) and maps the
//                new part; std::vector reallocates and copies
//    push_back, reserved   the same after reserve(N) on both
//    random read   M reads at the same pseudo-random indices of the same N
//                  elements, summing a field
//    flush       mapped_vector only: msync of everything written, then the
//                header (what durability costs; std::vector has none)
//
//  Both sides get the same elements and the same index sequence and return a
//  checksum; the program refuses to report if any pair disagrees. Best of 5,
//  alternating sides. The file is in $TMPDIR (or /tmp), on whatever disk that
//  is: the numbers include its page cache, not its device, except "flush".
//

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <unistd.h>

#include "mmap/mapped_vector.hpp"

namespace {

struct tick
{
    std::int64_t time;
    double price;
    std::int32_t size;
    std::int32_t venue;
};

tick make_tick(std::size_t i)
{
    return tick{static_cast<std::int64_t>(i), 100.0 + static_cast<double>(i % 977) * 0.01,
                static_cast<std::int32_t>(i % 500), static_cast<std::int32_t>(i % 7)};
}

using clock_type = std::chrono::steady_clock;
double seconds_since(clock_type::time_point t) { return std::chrono::duration<double>(clock_type::now() - t).count(); }

std::string bench_file()
{
    const char* tmp = std::getenv("TMPDIR");
    return std::string(tmp && *tmp ? tmp : "/tmp") + "/tsm_mmap_bench_" + std::to_string(::getpid()) + ".bin";
}

std::vector<std::uint32_t> indices(std::size_t n, std::size_t m)
{
    std::vector<std::uint32_t> ix(m);
    std::uint64_t s = 0x9e3779b97f4a7c15ull;
    for (auto& i : ix) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        i = static_cast<std::uint32_t>(s % n);
    }
    return ix;
}

volatile std::uint64_t sink;

} // namespace

int main(int argc, char** argv)
{
    const std::size_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 20'000'000;   // 480 MB of ticks
    const std::size_t m = 20'000'000;
    const auto ix = indices(n, m);
    const std::string file = bench_file();
    double best[2][4] = {{1e9, 1e9, 1e9, 1e9}, {1e9, 1e9, 1e9, 1e9}};   // [mapped, std][push, reserved push, read, flush]
    std::uint64_t sums[2][3] = {};

    for (int round = 0; round < 5; ++round) {
        for (int side = 0; side < 2; ++side) {
            const int which = (round % 2 == 0) ? side : 1 - side;   // alternate who goes first
            if (which == 0) {
                for (int reserved = 0; reserved < 2; ++reserved) {
                    auto v = snicholls::mapped_vector<tick>::create(file, {.reserve = std::size_t{1} << 36});
                    if (v.is_err()) { std::fprintf(stderr, "create: %s\n", v.unwrap_err_unchecked().message().c_str()); return 1; }
                    if (reserved && v->reserve(n).is_err()) return 1;
                    auto t = clock_type::now();
                    for (std::size_t i = 0; i < n; ++i)
                        if (v->push_back(make_tick(i)).is_err()) { std::fprintf(stderr, "push_back failed\n"); return 1; }
                    best[0][reserved] = std::min(best[0][reserved], seconds_since(t));
                    std::uint64_t s = 0;
                    for (const tick& k : *v) s += static_cast<std::uint64_t>(k.size);
                    sums[0][reserved] = s;
                    if (reserved) {
                        t = clock_type::now();
                        std::uint64_t r = 0;
                        for (std::uint32_t i : ix) r += static_cast<std::uint64_t>((*v)[i].time);
                        best[0][2] = std::min(best[0][2], seconds_since(t));
                        sums[0][2] = r;
                        t = clock_type::now();
                        if (v->flush().is_err()) return 1;
                        best[0][3] = std::min(best[0][3], seconds_since(t));
                    }
                }
                std::remove(file.c_str());
            } else {
                for (int reserved = 0; reserved < 2; ++reserved) {
                    std::vector<tick> v;
                    if (reserved) v.reserve(n);
                    auto t = clock_type::now();
                    for (std::size_t i = 0; i < n; ++i) v.push_back(make_tick(i));
                    best[1][reserved] = std::min(best[1][reserved], seconds_since(t));
                    std::uint64_t s = 0;
                    for (const tick& k : v) s += static_cast<std::uint64_t>(k.size);
                    sums[1][reserved] = s;
                    if (reserved) {
                        t = clock_type::now();
                        std::uint64_t r = 0;
                        for (std::uint32_t i : ix) r += static_cast<std::uint64_t>(v[i].time);
                        best[1][2] = std::min(best[1][2], seconds_since(t));
                        sums[1][2] = r;
                    }
                }
            }
        }
    }
    for (int k = 0; k < 3; ++k)
        if (sums[0][k] != sums[1][k]) {
            std::fprintf(stderr, "mmap_bench: fairness check FAILED (%d) - the checksums disagree; not reporting\n", k);
            return 1;
        }
    sink = sums[0][2];
    std::printf("mapped_vector<tick> vs std::vector<tick>, %zu elements (%.0f MB), best of 5, page %zu\n", n,
                static_cast<double>(n * sizeof(tick)) / 1e6, snicholls::mapped_region::page_size());
    std::printf("  %-22s %12s %12s %8s\n", "", "mapped", "std", "ratio");
    const char* names[3] = {"push_back", "push_back, reserved", "random read"};
    for (int k = 0; k < 2; ++k)
        std::printf("  %-22s %9.2f ns %9.2f ns %7.2fx\n", names[k], best[0][k] * 1e9 / static_cast<double>(n),
                    best[1][k] * 1e9 / static_cast<double>(n), best[0][k] / best[1][k]);
    std::printf("  %-22s %9.2f ns %9.2f ns %7.2fx\n", names[2], best[0][2] * 1e9 / static_cast<double>(m),
                best[1][2] * 1e9 / static_cast<double>(m), best[0][2] / best[1][2]);
    std::printf("  %-22s %9.0f ms  (msync of %.0f MB, then the header)\n", "flush", best[0][3] * 1e3,
                static_cast<double>(n * sizeof(tick)) / 1e6);
    std::printf("fairness: every checksum agrees\n");
    return 0;
}
