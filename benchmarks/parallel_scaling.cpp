//
//  parallel_scaling.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - what does parallel_for actually buy?
//
//  A parallel loop is the easiest thing in this library to believe in without
//  evidence, and the easiest to be wrong about. The interesting questions are
//  not "is it faster" but:
//
//    1. How far does it scale, and what stops it? A compute-bound body should
//       track the core count. A memory-bound one should stop scaling early,
//       because the limit is DRAM bandwidth and adding cores does not add
//       bandwidth. Reporting only the first would be advertising.
//    2. What does a parallel_for CALL cost? There is a floor - submits, atomic
//       claims, a gate - and below some range size the loop is slower than
//       just doing the work. That crossover is the number a caller needs and
//       is almost never published.
//    3. Does grain size matter, and which way?
//    4. Does hand unrolling help? utils/constexpr_for.hpp says probably not at
//       -O2, and owes a measurement rather than an opinion.
//
//  Every row is the best of several timed runs (best, not mean: we are after
//  the machine's capability, and noise only ever adds). Serial is the same
//  loop written plainly, compiled in the same TU at the same optimisation.
//

#include "../TSMoveables/concurrent/parallel_for.hpp"
#include "../TSMoveables/concurrent/thread_pool.hpp"
#include "../TSMoveables/utils/constexpr_for.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace snicholls;
using clk = std::chrono::steady_clock;

namespace {

double ms_of(clk::duration d)
{
    return std::chrono::duration<double, std::milli>(d).count();
}

// Best of N runs.
template <typename F>
double best_ms(int runs, F&& f)
{
    double best = 1e30;
    for (int r = 0; r < runs; ++r) {
        const auto t0 = clk::now();
        f();
        const double ms = ms_of(clk::now() - t0);
        best = (ms < best) ? ms : best;
    }
    return best;
}

void rule(const char* title)
{
    std::printf("\n%s\n", title);
    std::printf("  %s\n", std::string(74, '-').c_str());
}

// ---------------------------------------------------------------- workloads
//
// Compute-bound: transcendental math per element, nothing touched twice. The
// case parallelism is supposed to win.
struct compute_body {
    double* out;
    const double* in;
    void operator()(std::size_t i) const
    {
        double x = in[i];
        for (int k = 0; k < 12; ++k)
            x = std::sin(x) * 1.000001 + std::sqrt(x * x + 1.0);
        out[i] = x;
    }
};

// Memory-bound: one multiply-add per element over arrays far larger than
// cache. The case where cores are not the scarce resource.
struct memory_body {
    double* out;
    const double* a;
    const double* b;
    void operator()(std::size_t i) const { out[i] = a[i] * 1.5 + b[i]; }
};

} // namespace

int main(int argc, char** argv)
{
    // --quick shrinks the ranges for CI: shared runners have few cores, less
    // memory and a time budget, and the shape of the curve survives the cut
    // even though the absolute numbers do not. --markdown fences the output so
    // it renders inside a GitHub step summary.
    bool quick = false, markdown = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--quick") quick = true;
        else if (a == "--markdown") { markdown = true; quick = true; }
    }
    const int shift = quick ? 4 : 0;        // 16x fewer elements
    const int runs  = quick ? 2 : 3;
    if (markdown) std::printf("```\n");

    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    std::printf("parallel_for scaling - %u hardware threads\n", hw);

    // ------------------------------------------------------------ scaling
    //
    // Two bodies, the same shape of loop, swept over worker counts.
    {
        const std::size_t n_compute = std::size_t{1} << (20 - shift);
        const std::size_t n_memory  = std::size_t{1} << (24 - shift);

        std::vector<double> in(n_compute), out(n_compute);
        for (std::size_t i = 0; i < n_compute; ++i)
            in[i] = double(i % 1000) * 0.001;

        std::vector<double> ma(n_memory), mb(n_memory), mo(n_memory);
        for (std::size_t i = 0; i < n_memory; ++i) { ma[i] = double(i); mb[i] = double(i) * 0.5; }

        const compute_body cb{out.data(), in.data()};
        const memory_body  mbod{mo.data(), ma.data(), mb.data()};

        const double serial_compute = best_ms(runs, [&] {
            for (std::size_t i = 0; i < n_compute; ++i) cb(i);
        });
        const double serial_memory = best_ms(runs, [&] {
            for (std::size_t i = 0; i < n_memory; ++i) mbod(i);
        });

        rule("compute-bound (1M elements, 12 transcendental ops each)");
        std::printf("  %-10s %12s %12s %10s %12s\n",
                    "workers", "time ms", "speedup", "efficiency", "vs serial");
        std::printf("  %-10s %12.2f %12s %10s %12s\n", "serial", serial_compute, "1.00x", "-", "baseline");
        for (unsigned w = 1; w <= hw; w *= 2) {
            work_stealing_task_pool pool(w);
            const double t = best_ms(runs, [&] {
                parallel_for(pool, std::size_t{0}, n_compute, cb);
            });
            std::printf("  %-10u %12.2f %11.2fx %9.0f%% %12s\n",
                        w, t, serial_compute / t, 100.0 * (serial_compute / t) / double(w),
                        t < serial_compute ? "faster" : "SLOWER");
        }

        rule("memory-bound (16M elements, one multiply-add each)");
        std::printf("  %-10s %12s %12s %10s %12s\n",
                    "workers", "time ms", "speedup", "efficiency", "GB/s");
        std::printf("  %-10s %12.2f %12s %10s %12.1f\n", "serial", serial_memory, "1.00x", "-",
                    (3.0 * double(n_memory) * sizeof(double) / 1e9) / (serial_memory / 1e3));
        for (unsigned w = 1; w <= hw; w *= 2) {
            work_stealing_task_pool pool(w);
            const double t = best_ms(runs, [&] {
                parallel_for(pool, std::size_t{0}, n_memory, mbod);
            });
            std::printf("  %-10u %12.2f %11.2fx %9.0f%% %12.1f\n",
                        w, t, serial_memory / t, 100.0 * (serial_memory / t) / double(w),
                        (3.0 * double(n_memory) * sizeof(double) / 1e9) / (t / 1e3));
        }
        std::printf("\n  The second table is the honest one: cores do not add memory bandwidth,\n"
                    "  so a streaming body stops scaling long before it runs out of threads.\n"
                    "  Efficiency above 100%% at 2-4 workers is not superlinear magic - it is\n"
                    "  cache residency: each worker's slice fits a private cache level that the\n"
                    "  whole array did not. Efficiency past the physical core count falls off\n"
                    "  because the remaining 'threads' are SMT siblings sharing one core's\n"
                    "  execution units, not new cores.\n");
    }

    // ------------------------------------------------------- the crossover
    //
    // Where does a parallel_for call start paying for itself? Below some size
    // the machinery costs more than the work.
    {
        rule("call overhead - where parallel stops being worth it (compute body)");
        std::printf("  %-12s %14s %14s %12s\n", "elements", "serial ms", "parallel ms", "verdict");
        work_stealing_task_pool pool(hw);
        const std::size_t cap = std::size_t{1} << (16 - shift);
        std::vector<double> in(cap), out(cap);
        for (std::size_t i = 0; i < in.size(); ++i) in[i] = double(i % 1000) * 0.001;
        const compute_body cb{out.data(), in.data()};

        for (std::size_t n = 16; n <= cap; n *= 8) {
            const int inner = quick ? 50 : 200;
            const double s = best_ms(inner, [&] {
                for (std::size_t i = 0; i < n; ++i) cb(i);
            });
            const double p = best_ms(inner, [&] {
                parallel_for(pool, std::size_t{0}, n, cb);
            });
            std::printf("  %-12zu %14.4f %14.4f %12s\n", n, s, p,
                        p < s ? "parallel" : "serial wins");
        }
    }

    // ---------------------------------------------------------- grain size
    {
        rule("grain size (compute body, 1M elements, all cores)");
        std::printf("  %-14s %14s %12s\n", "grain", "time ms", "note");
        work_stealing_task_pool pool(hw);
        const std::size_t n = std::size_t{1} << (20 - shift);
        std::vector<double> in(n), out(n);
        for (std::size_t i = 0; i < n; ++i) in[i] = double(i % 1000) * 0.001;
        const compute_body cb{out.data(), in.data()};

        const std::size_t grains[] = {0, 1, 64, 1024, 16384, n};
        for (std::size_t g : grains) {
            const double t = best_ms(runs, [&] { parallel_for(pool, std::size_t{0}, n, cb, g); });
            const char* note = (g == 0)   ? "default (~4 chunks/worker)"
                             : (g == 1)   ? "one element per chunk"
                             : (g == n)   ? "one chunk total - no parallelism"
                                          : "";
            std::printf("  %-14zu %14.2f %12s\n", g, t, note);
        }
    }

    // ------------------------------------------------------- unrolling
    //
    // utils/constexpr_for.hpp claims hand unrolling is usually a wash at -O2.
    // This is where that claim gets to be wrong in public.
    {
        rule("hand unrolling vs letting the compiler do it (memory body, 16M)");
        std::printf("  %-16s %14s %12s\n", "unroll factor", "time ms", "vs none");
        work_stealing_task_pool pool(hw);
        const std::size_t n = std::size_t{1} << (24 - shift);
        std::vector<double> a(n), b(n), o(n);
        for (std::size_t i = 0; i < n; ++i) { a[i] = double(i); b[i] = double(i) * 0.5; }
        const memory_body mb{o.data(), a.data(), b.data()};

        const double base = best_ms(runs, [&] { parallel_for<1>(pool, std::size_t{0}, n, mb); });
        std::printf("  %-16s %14.2f %12s\n", "1 (none)", base, "baseline");
        const double u2 = best_ms(runs, [&] { parallel_for<2>(pool, std::size_t{0}, n, mb); });
        std::printf("  %-16s %14.2f %11.2fx\n", "2", u2, base / u2);
        const double u4 = best_ms(runs, [&] { parallel_for<4>(pool, std::size_t{0}, n, mb); });
        std::printf("  %-16s %14.2f %11.2fx\n", "4", u4, base / u4);
        const double u8 = best_ms(runs, [&] { parallel_for<8>(pool, std::size_t{0}, n, mb); });
        std::printf("  %-16s %14.2f %11.2fx\n", "8", u8, base / u8);
        const double u16 = best_ms(runs, [&] { parallel_for<16>(pool, std::size_t{0}, n, mb); });
        std::printf("  %-16s %14.2f %11.2fx\n", "16", u16, base / u16);

        const double best = std::min({base, u2, u4, u8, u16});
        std::printf("\n  Spread across all factors: %.1f%%. Anything inside a few percent is\n"
                    "  noise, not a win - the compiler already unrolled this loop.\n",
                    100.0 * (std::max({base, u2, u4, u8, u16}) - best) / best);
    }

    // ------------------------------------------- what unrolling IS good for
    {
        rule("constexpr_for - unrolling that a compiler cannot do for you");
        const std::size_t n = std::size_t{1} << (22 - shift);
        std::vector<double> m(n * 0 + 16, 1.0);     // a 4x4 tile, reused
        double acc = 0;

        // A fixed 4x4 kernel with every index a compile-time constant: no
        // loop, no bounds arithmetic, and the body can use I/J as template
        // arguments. A run-time loop cannot be turned into this by -O2 alone.
        const double t = best_ms(quick ? 10 : 50, [&] {
            for (std::size_t r = 0; r < n / 16; ++r)
                constexpr_nest<4, 4>([&](auto I, auto J) {
                    acc += m[I.value * 4 + J.value];
                });
        });
        std::printf("  4x4 constexpr_nest over %zu tiles: %.2f ms  (acc=%.0f)\n", n / 16, t, acc);
        std::printf("  The value here is not speed - it is that I and J are constant\n"
                    "  expressions, so std::get<I>, template arguments and fixed-size\n"
                    "  kernels become possible at all.\n");
    }

    if (markdown) std::printf("```\n");
    std::printf("\ndone\n");
    return 0;
}
