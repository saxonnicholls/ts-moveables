//
//  pool_dispatch.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - what does calling a pool through task_pool& cost?
//
//  The same pool, the same work, two call paths:
//
//    concrete  the pool's own type. parallel_for and async are templates over
//              task_pool_like, so their calls are direct; work_stealing_ and
//              inline_task_pool take a submitted callable as it is, through
//              their submit(F&&) (concepts/task_pool.hpp, pool_task).
//    dynamic   the same pool as task_pool&, its dynamic type hidden from the
//              compiler: every call is virtual and every task a std::function.
//
//  Three workloads: parallel_for over 4096 ints (one task per worker, then
//  chunks claimed from a cursor), submitting 200,000 tasks with an 8-byte
//  capture (inside std::function's small buffer), and 200,000 with a 48-byte
//  capture (past it, so std::function allocates).
//
//  The two paths are interleaved rep by rep, alternating which goes first:
//  measured one after the other, whichever ran second looked faster on a
//  loaded machine, by up to 24%. Best of 15. Run it on a quiet machine.
//
//      make bench-pool-dispatch
//

#include "../TSMoveables/concurrent/parallel_for.hpp"
#include "../TSMoveables/concurrent/thread_pool.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace snicholls;
using clk = std::chrono::steady_clock;

namespace
{
    // Through a volatile function pointer, so the compiler cannot see that the
    // task_pool& is a final concrete pool and devirtualise the calls.
    task_pool& identity(task_pool& p) { return p; }
    task_pool& (*volatile erase)(task_pool&) = identity;

    template <typename A, typename B>
    void interleaved(A a, B b, double& best_a, double& best_b, int reps)
    {
        best_a = best_b = 1e300;
        const auto time = [](auto& f) {
            const auto t0 = clk::now();
            f();
            return std::chrono::duration<double, std::nano>(clk::now() - t0).count();
        };
        for (int r = 0; r < reps; ++r) {
            if (r % 2 != 0) {
                best_b = std::min(best_b, time(b));
                best_a = std::min(best_a, time(a));
            } else {
                best_a = std::min(best_a, time(a));
                best_b = std::min(best_b, time(b));
            }
        }
    }

    void row(double concrete, double dynamic)
    {
        std::printf(" %8.1f | %8.1f | %5.2fx |", concrete, dynamic, dynamic / concrete);
    }

    template <typename Pool>
    void run(const char* name, Pool& pool)
    {
        task_pool& dyn = erase(pool);
        constexpr int loops = 20000, subs = 200000, reps = 15;

        std::vector<int> v(4096);
        const auto loop = [&](auto& p) {
            for (int k = 0; k < loops; ++k)
                parallel_for(p, 0, 4096, [&](int i) { v[static_cast<std::size_t>(i)] += i; });
        };
        std::atomic<long> sink{0};
        const auto small = [&](auto& p) {
            for (int k = 0; k < subs; ++k)
                p.submit([&sink] { sink.fetch_add(1, std::memory_order_relaxed); });
            p.wait_idle();
        };
        long a = 1, b = 2, c = 3, d = 4, e = 5;   // with &sink, 48 bytes (not const: a constant is not captured)
        const auto large = [&](auto& p) {
            for (int k = 0; k < subs; ++k)
                p.submit([&sink, a, b, c, d, e] { sink.fetch_add(a + b + c + d + e, std::memory_order_relaxed); });
            p.wait_idle();
        };

        double s1, d1, s2, d2, s3, d3;
        interleaved([&] { loop(pool); }, [&] { loop(dyn); }, s1, d1, reps);
        interleaved([&] { small(pool); }, [&] { small(dyn); }, s2, d2, reps);
        interleaved([&] { large(pool); }, [&] { large(dyn); }, s3, d3, reps);

        std::printf("| `%s` |", name);
        row(s1 / loops, d1 / loops);
        row(s2 / subs, d2 / subs);
        row(s3 / subs, d3 / subs);
        std::printf("\n");
    }
} // namespace

int main()
{
    const unsigned hw = std::thread::hardware_concurrency();
    const std::size_t workers = hw > 1 ? hw - 1 : 1;   // the caller drains parallel_for chunks too
    std::printf("Pool dispatch: concrete type against task_pool&, %zu workers, ns per call (best of 15, interleaved)\n\n", workers);
    std::printf("| pool | parallel_for(4096) concrete | dynamic | ratio | submit 8 B capture concrete | dynamic | ratio | submit 48 B capture concrete | dynamic | ratio |\n");
    std::printf("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n");
    { work_stealing_task_pool p(workers); run("work_stealing_task_pool", p); }
    { mpmc_task_pool p(workers); run("mpmc_task_pool", p); }
    { sharded_task_pool p(workers); run("sharded_task_pool", p); }
    { mutex_task_pool p(workers); run("mutex_task_pool", p); }
    { inline_task_pool p; run("inline_task_pool", p); }
    return 0;
}
