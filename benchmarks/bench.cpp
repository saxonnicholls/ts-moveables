//
//  bench.cpp
//  TSMoveables
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - benchmark harness
//
//  Dependency-free by design, like everything else here. Each case pushes a
//  fixed number of items through a producer/consumer pair on two threads and
//  reports throughput. Every case is run several times and the best run is
//  reported (standard practice for throughput microbenchmarks - the best run
//  is the one least disturbed by the machine).
//
//  Build and run with `make bench` (compiled -O3 -DNDEBUG).
//
//  These numbers are one machine's numbers. They ground relative claims
//  (ring vs mutex vs spin lock on the same box); they are not a cross-library
//  shootout - see FUTURE_DIRECTIONS.md.
//

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <iterator>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "../TSMoveables/ts_moveables.hpp"

using namespace snicholls;

namespace {

constexpr int repeats = 5;
constexpr std::int64_t total_items = 2'000'000;

volatile std::int64_t sink = 0;             // keeps sums observable - defeats DCE
bool markdown = false;                      // --markdown: emit a GitHub-flavoured table

template <typename F>
double best_seconds(F&& f)
{
    double best = 1e300;
    for (int r = 0; r < repeats; ++r) {
        const auto t0 = std::chrono::steady_clock::now();
        f();
        const auto t1 = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double>(t1 - t0).count());
    }
    return best;
}

void report(const char* name, double seconds, std::int64_t items)
{
    const double mops = static_cast<double>(items) / seconds / 1e6;
    const double ns = seconds / static_cast<double>(items) * 1e9;
    if (markdown)
        std::printf("| `%s` | %.2f Mops/s | %.1f ns |\n", name, mops, ns);
    else
        std::printf("  %-42s %9.2f Mops/s  %8.1f ns/op\n", name, mops, ns);
}

// ------------------------------------------------------------ ring, singles

void bench_ring_singles()
{
    const double s = best_seconds([] {
        circular_buffer<std::int64_t> ring{1024};
        std::thread producer([&] {
            for (std::int64_t i = 0; i < total_items;) {
                if (ring.try_push(i))
                    ++i;
            }
        });
        std::int64_t sum = 0;
        for (std::int64_t received = 0; received < total_items;) {
            if (auto v = ring.try_pop()) {
                sum += *v;
                ++received;
            }
        }
        producer.join();
        sink = sum;
    });
    report("circular_buffer<T>  try_push/try_pop", s, total_items);
}

void bench_ring_static_singles()
{
    const double s = best_seconds([] {
        circular_buffer<std::int64_t, 1024> ring;
        std::thread producer([&] {
            for (std::int64_t i = 0; i < total_items;) {
                if (ring.try_push(i))
                    ++i;
            }
        });
        std::int64_t sum = 0;
        for (std::int64_t received = 0; received < total_items;) {
            if (auto v = ring.try_pop()) {
                sum += *v;
                ++received;
            }
        }
        producer.join();
        sink = sum;
    });
    report("circular_buffer<T, N>  try_push/try_pop", s, total_items);
}

// ------------------------------------------------------------- ring, batch

void bench_ring_batch()
{
    // static: not captured by the lambdas (MSVC C3493) yet still a constant
    // expression inside them (MSVC C2131)
    static constexpr std::size_t batch = 64;
    const double s = best_seconds([] {
        circular_buffer<std::int64_t> ring{1024};
        std::thread producer([&] {
            std::int64_t chunk[batch];
            std::int64_t next = 0;
            while (next < total_items) {
                const std::size_t want = static_cast<std::size_t>(
                    std::min<std::int64_t>(batch, total_items - next));
                for (std::size_t i = 0; i < want; ++i)
                    chunk[i] = next + static_cast<std::int64_t>(i);
                std::size_t pushed = 0;
                while (pushed < want)
                    pushed += ring.push_n(chunk + pushed, want - pushed);
                next += static_cast<std::int64_t>(want);
            }
        });
        std::int64_t sum = 0;
        std::vector<std::int64_t> out;
        out.reserve(batch);
        for (std::int64_t received = 0; received < total_items;) {
            out.clear();
            const std::size_t n = ring.pop_n(std::back_inserter(out), batch);
            for (std::size_t i = 0; i < n; ++i)
                sum += out[i];
            received += static_cast<std::int64_t>(n);
        }
        producer.join();
        sink = sum;
    });
    report("circular_buffer<T>  push_n/pop_n (64)", s, total_items);
}

// ------------------------------------------------- baseline: mutex + queue

void bench_mutex_queue()
{
    const double s = best_seconds([] {
        std::mutex m;
        std::queue<std::int64_t> q;
        std::thread producer([&] {
            for (std::int64_t i = 0; i < total_items;) {
                std::lock_guard<std::mutex> lock(m);
                if (q.size() < 1024) {
                    q.push(i);
                    ++i;
                }
            }
        });
        std::int64_t sum = 0;
        for (std::int64_t received = 0; received < total_items;) {
            std::lock_guard<std::mutex> lock(m);
            if (!q.empty()) {
                sum += q.front();
                q.pop();
                ++received;
            }
        }
        producer.join();
        sink = sum;
    });
    report("std::mutex + std::queue", s, total_items);
}

// --------------------------------------------- baseline: spin lock + queue

void bench_spin_queue()
{
    const double s = best_seconds([] {
        moveable_spin_lock m;
        std::queue<std::int64_t> q;
        std::thread producer([&] {
            for (std::int64_t i = 0; i < total_items;) {
                std::lock_guard<moveable_spin_lock> lock(m);
                if (q.size() < 1024) {
                    q.push(i);
                    ++i;
                }
            }
        });
        std::int64_t sum = 0;
        for (std::int64_t received = 0; received < total_items;) {
            std::lock_guard<moveable_spin_lock> lock(m);
            if (!q.empty()) {
                sum += q.front();
                q.pop();
                ++received;
            }
        }
        producer.join();
        sink = sum;
    });
    report("moveable_spin_lock + std::queue", s, total_items);
}

// ------------------------------- baseline: synchronized_waitable (cv-based)

void bench_synchronized_queue()
{
    // Deliberately the convenient, blocking style - this is what the ring is
    // faster than, and what you should still use when convenience wins
    static constexpr std::int64_t items = total_items / 10;    // cv wakeups are slow; keep the run short
    const double s = best_seconds([] {
        synchronized_waitable<std::queue<std::int64_t>> q;
        std::thread producer([&] {
            for (std::int64_t i = 0; i < items; ++i)
                q.update([i](std::queue<std::int64_t>& qu) { qu.push(i); });
        });
        std::int64_t sum = 0;
        for (std::int64_t received = 0; received < items;) {
            q.wait_then([](const std::queue<std::int64_t>& qu) { return !qu.empty(); },
                        [&](std::queue<std::int64_t>& qu) {
                            while (!qu.empty()) {
                                sum += qu.front();
                                qu.pop();
                                ++received;
                            }
                        });
        }
        producer.join();
        sink = sum;
    });
    report("synchronized_waitable<std::queue> (cv)", s, items);
}

// ---------------------------------------------------------------- disruptor

void bench_disruptor()
{
    const double s = best_seconds([] {
        disruptor<std::int64_t> d{1024};
        auto& consumer = d.add_consumer();
        std::thread producer([&] {
            for (std::int64_t i = 0; i < total_items; ++i)
                d.publish([i](std::int64_t& e) { e = i; });
        });
        std::int64_t sum = 0;
        std::int64_t received = 0;
        while (received < total_items)
            received += static_cast<std::int64_t>(
                consumer.poll([&](std::int64_t& e, std::int64_t, bool) { sum += e; }));
        producer.join();
        sink = sum;
    });
    report("disruptor<T>  publish/poll", s, total_items);
}

// -------------------------------------------------------------- thread pools
// A different metric: submit N trivial tasks and wait for them all. This
// measures the pool's submit + dispatch + completion overhead per task, not
// SPSC hand-off, so it lives in its own table. Fewer tasks than the SPSC cases
// because the mutex baseline notifies on every submit.

constexpr std::int64_t pool_tasks = 500'000;

void report_pool(const char* name, double seconds)
{
    const double mtps = static_cast<double>(pool_tasks) / seconds / 1e6;
    const double ns = seconds / static_cast<double>(pool_tasks) * 1e9;
    if (markdown)
        std::printf("| `%s` | %.2f M tasks/s | %.0f ns/task |\n", name, mtps, ns);
    else
        std::printf("  %-42s %9.2f M tasks/s %8.0f ns/task\n", name, mtps, ns);
}

template <typename Pool>
double pool_run(Pool& pool)
{
    return best_seconds([&pool] {
        std::atomic<std::int64_t> acc{0};
        for (std::int64_t i = 0; i < pool_tasks; ++i)
            pool.submit([&acc] { acc.fetch_add(1, std::memory_order_relaxed); });
        pool.wait_idle();
        sink = acc.load();
    });
}

void bench_thread_pools()
{
    {
        mutex_task_pool p{4};
        report_pool("mutex_task_pool (submit+run)", pool_run(p));
    }
    {
        sharded_task_pool p{4};
        report_pool("sharded_task_pool (submit+run)", pool_run(p));
    }
    {
        dispatch_task_pool p{4, 4096};              // single-dispatcher: bench submits from one thread
        report_pool("dispatch_task_pool (submit+run)", pool_run(p));
    }
    {
        mpmc_task_pool p{4};
        report_pool("mpmc_task_pool (submit+run)", pool_run(p));
    }
    {
        work_stealing_task_pool p{4};
        report_pool("work_stealing_task_pool (submit+run)", pool_run(p));
    }
    // std::async yardstick - launches a thread per task, so a tiny count only
    {
        static constexpr std::int64_t async_tasks = 20'000;    // static: not captured (MSVC C3493)
        const double s = best_seconds([] {
            std::atomic<std::int64_t> acc{0};
            std::vector<std::future<void>> futs;
            futs.reserve(async_tasks);
            for (std::int64_t i = 0; i < async_tasks; ++i)
                futs.push_back(std::async(std::launch::async,
                                          [&acc] { acc.fetch_add(1, std::memory_order_relaxed); }));
            for (auto& f : futs)
                f.get();
            sink = acc.load();
        });
        const double mtps = static_cast<double>(async_tasks) / s / 1e6;
        if (markdown)
            std::printf("| `std::async (thread per task, 20k)` | %.2f M tasks/s | %.0f ns/task |\n",
                        mtps, s / static_cast<double>(async_tasks) * 1e9);
        else
            std::printf("  %-42s %9.2f M tasks/s %8.0f ns/task\n",
                        "std::async (thread per task, 20k)", mtps,
                        s / static_cast<double>(async_tasks) * 1e9);
    }
}

// Fork-join: spreader tasks spawn many sub-tasks from inside a worker. This is
// where work-stealing earns its name - spawned tasks hit hot local deques
// instead of one contended global queue. (Only pools safe for submit-from-
// within-a-task: work-stealing's inline-on-saturation and the unbounded mutex
// queue. The bounded mpmc/dispatch pools are not built for this shape.)
constexpr std::int64_t fj_spreaders = 32;
constexpr std::int64_t fj_per = 2048;
constexpr std::int64_t fj_total = fj_spreaders * (fj_per + 1);

template <typename Pool>
double forkjoin_run(Pool& pool)
{
    return best_seconds([&pool] {
        std::atomic<std::int64_t> acc{0};
        for (std::int64_t s = 0; s < fj_spreaders; ++s)
            pool.submit([&pool, &acc] {
                for (std::int64_t i = 0; i < fj_per; ++i)
                    pool.submit([&acc] { acc.fetch_add(1, std::memory_order_relaxed); });
            });
        pool.wait_idle();
        sink = acc.load();
    });
}

void report_fj(const char* name, double seconds)
{
    const double mtps = static_cast<double>(fj_total) / seconds / 1e6;
    if (markdown)
        std::printf("| `%s` | %.2f M tasks/s | %.0f ns/task |\n", name, mtps,
                    seconds / static_cast<double>(fj_total) * 1e9);
    else
        std::printf("  %-42s %9.2f M tasks/s %8.0f ns/task\n", name, mtps,
                    seconds / static_cast<double>(fj_total) * 1e9);
}

void bench_forkjoin()
{
    {
        work_stealing_task_pool p{4};
        report_fj("work_stealing_task_pool (fork-join)", forkjoin_run(p));
    }
    {
        mutex_task_pool p{4};
        report_fj("mutex_task_pool (fork-join)", forkjoin_run(p));
    }
}

void bench_disruptor_batch()
{
    // static: not captured by the lambdas (MSVC C3493) yet still a constant
    // expression inside them (MSVC C2131)
    static constexpr std::size_t batch = 64;
    const double s = best_seconds([] {
        disruptor<std::int64_t> d{1024};
        auto& consumer = d.add_consumer();
        std::thread producer([&] {
            for (std::int64_t i = 0; i < total_items; i += static_cast<std::int64_t>(batch))
                d.publish_n(batch, [](std::int64_t& e, std::int64_t seq) { e = seq; });
        });
        std::int64_t sum = 0;
        std::int64_t received = 0;
        while (received < total_items)
            received += static_cast<std::int64_t>(
                consumer.poll([&](std::int64_t& e, std::int64_t, bool) { sum += e; }));
        producer.join();
        sink = sum;
    });
    report("disruptor<T>  publish_n/poll (64)", s, total_items);
}

} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--markdown") == 0)
            markdown = true;

    if (markdown) {
        std::printf("### `make bench` - SPSC, %lldM items, 1 producer + 1 consumer thread, best of %d\n\n",
                    static_cast<long long>(total_items / 1'000'000), repeats);
        std::printf("| Case | Throughput | Per op |\n|---|---|---|\n");
    } else {
        std::printf("TSMoveables benchmarks - %lld items per case, best of %d runs\n",
                    static_cast<long long>(total_items), repeats);
        std::printf("SPSC: one producer thread, one consumer thread\n\n");
    }

    bench_ring_singles();
    bench_ring_static_singles();
    bench_ring_batch();
    bench_disruptor();
    bench_disruptor_batch();
    bench_mutex_queue();
    bench_spin_queue();
    bench_synchronized_queue();

    if (markdown)
        std::printf("\n### thread pools - %lldk trivial tasks, submit + wait_idle, best of %d\n\n"
                    "| Pool | Throughput | Per task |\n|---|---|---|\n",
                    static_cast<long long>(pool_tasks / 1000), repeats);
    else
        std::printf("\nthread pools - %lld trivial tasks, submit + wait_idle:\n", pool_tasks);
    bench_thread_pools();

    if (markdown)
        std::printf("\n### fork-join - %lldk tasks spawned from inside workers, best of %d\n\n"
                    "| Pool | Throughput | Per task |\n|---|---|---|\n",
                    static_cast<long long>(fj_total / 1000), repeats);
    else
        std::printf("\nfork-join - %lld tasks spawned from inside workers:\n", fj_total);
    bench_forkjoin();

    if (!markdown)
        std::printf("\n(sink=%lld)\n", static_cast<long long>(sink));
    return 0;
}
