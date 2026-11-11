//
//  tests_thread_pool.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - unit tests for the thread pool family
//

#include "test_helpers.hpp"

#include <atomic>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../TSMoveables/concurrent/thread_pool.hpp"
#include "../TSMoveables/concurrent/parallel_for.hpp"
#include "../TSMoveables/concepts/task_pool.hpp"

using namespace snicholls;

namespace {

// Exercise one concrete pool through the shared expectations. dispatch_task_pool
// is single-dispatcher, so submission always happens from this (one) thread.
template <typename Pool>
void exercise(Pool&& pool, const char* name)
{
    // wait_idle on an empty pool returns at once
    pool.wait_idle();
    assert(pool.worker_count() >= 1);

    // Every submitted task runs exactly once
    constexpr int n = 20000;
    std::atomic<long long> sum{0};
    std::atomic<int> ran{0};
    for (int i = 0; i < n; ++i)
        pool.submit([i, &sum, &ran] {
            sum.fetch_add(i, std::memory_order_relaxed);
            ran.fetch_add(1, std::memory_order_relaxed);
        });
    pool.wait_idle();
    assert(ran.load() == n);
    assert(sum.load() == static_cast<long long>(n) * (n - 1) / 2);

    // Pool is reusable after wait_idle
    std::atomic<int> again{0};
    for (int i = 0; i < 1000; ++i)
        pool.submit([&again] { again.fetch_add(1, std::memory_order_relaxed); });
    pool.wait_idle();
    assert(again.load() == 1000);

    // async: result-returning submission over the interface
    task_pool& iface = pool;
    auto f1 = async(iface, [] { return 6 * 7; });
    auto f2 = async(iface, [] { return std::string("done"); });
    assert(f1.get() == 42);
    assert(f2.get() == "done");

    // Exceptions surface through the future, pool stays healthy
    auto f3 = async(iface, []() -> int { throw std::runtime_error("boom"); });
    bool threw = false;
    try {
        (void)f3.get();   // newer libc++ marks future::get [[nodiscard]]
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
    auto f4 = async(iface, [] { return 1; });
    assert(f4.get() == 1);

    pass(name);
}

void test_mutex_pool()
{
    exercise(mutex_task_pool{4}, "thread pool: mutex_task_pool");
}

void test_sharded_pool()
{
    exercise(sharded_task_pool{4}, "thread pool: sharded_task_pool");
}

void test_dispatch_pool()
{
    exercise(dispatch_task_pool{4, 256}, "thread pool: dispatch_task_pool");
}

void test_mpmc_pool()
{
    exercise(mpmc_task_pool{4}, "thread pool: mpmc_task_pool");
}

void test_work_stealing_pool()
{
    exercise(work_stealing_task_pool{4}, "thread pool: work_stealing_task_pool");
}

void test_work_stealing_forkjoin()
{
    // Spreader tasks each submit many leaf tasks from inside a worker, so the
    // leaves land in that worker's local deque - idle workers must steal them.
    // Exercises local push + injector + stealing paths together.
    work_stealing_task_pool pool{4};
    std::atomic<long long> leaves{0};
    // static: read inside lambdas, so not captured (MSVC C3493) yet still a
    // constant expression there
    static constexpr int spreaders = 8;
    static constexpr int per_spreader = 5000;

    for (int s = 0; s < spreaders; ++s)
        pool.submit([&pool, &leaves] {
            for (int i = 0; i < per_spreader; ++i)
                pool.submit([&leaves] { leaves.fetch_add(1, std::memory_order_relaxed); });
        });
    pool.wait_idle();
    assert(leaves.load() == static_cast<long long>(spreaders) * per_spreader);

    // Recursive fork: parallel sum of 0..n-1 by divide and conquer, results
    // combined through atomics. Deep local-deque nesting plus stealing.
    work_stealing_task_pool pool2{4};
    std::atomic<long long> total{0};
    static constexpr int n = 100000;
    static constexpr int chunk = 500;
    for (int start = 0; start < n; start += chunk)
        pool2.submit([&total, start] {
            long long partial = 0;
            for (int i = start; i < start + chunk && i < n; ++i)
                partial += i;
            total.fetch_add(partial, std::memory_order_relaxed);
        });
    pool2.wait_idle();
    assert(total.load() == static_cast<long long>(n) * (n - 1) / 2);

    pass("thread pool: work-stealing fork/join with stealing");
}

void test_polymorphic_use()
{
    // The interface is the comparison surface: drive several implementations
    // through task_pool& with identical code
    std::vector<std::unique_ptr<task_pool>> pools;
    pools.emplace_back(std::make_unique<mutex_task_pool>(2));
    pools.emplace_back(std::make_unique<sharded_task_pool>(3));

    for (auto& p : pools) {
        std::atomic<long long> total{0};
        constexpr int n = 5000;
        for (int i = 0; i < n; ++i)
            p->submit([&total] { total.fetch_add(1, std::memory_order_relaxed); });
        p->wait_idle();
        assert(total.load() == n);
    }

    pass("thread pool: polymorphic use through task_pool&");
}

void test_moveability()
{
    // A pool handle moves; workers reference the stable heap core, so in-flight
    // work is unaffected and the moved-to handle keeps working
    mutex_task_pool a{3};
    std::atomic<int> ran{0};
    for (int i = 0; i < 500; ++i)
        a.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });

    mutex_task_pool b(std::move(a));                // move mid-flight
    for (int i = 0; i < 500; ++i)
        b.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
    b.wait_idle();
    assert(ran.load() == 1000);
    assert(b.worker_count() == 3);

    // Move-assignment
    mutex_task_pool c{1};
    c = std::move(b);
    std::atomic<int> more{0};
    c.submit([&more] { more.fetch_add(1, std::memory_order_relaxed); });
    c.wait_idle();
    assert(more.load() == 1);

    pass("thread pool: moveable handle");
}

void test_reentrant_submit()
{
    // A task that submits more tasks - the join counter must account for work
    // discovered during execution. (Not the dispatch pool: its single-
    // dispatcher contract forbids submitting from worker threads.)
    mutex_task_pool pool{4};
    std::atomic<int> done{0};
    constexpr int fanout = 100;

    for (int i = 0; i < fanout; ++i)
        pool.submit([&pool, &done] {
            for (int j = 0; j < 10; ++j)
                pool.submit([&done] { done.fetch_add(1, std::memory_order_relaxed); });
        });
    // A single wait_idle suffices: each child is counted (add) while its parent
    // is still outstanding, so the count cannot reach zero until the children
    // have run too
    pool.wait_idle();
    assert(done.load() == fanout * 10);

    pass("thread pool: reentrant submission");
}

} // namespace

// Every pool, and the interface itself, has task_pool's members; other types
// do not. Checked as the C++17 trait always and as the concept under C++20.
struct worker_count_may_throw {
    void submit(task_pool::task) {}
    void wait_idle() {}
    std::size_t worker_count() const { return 1; }   // not noexcept: not a task_pool
};
static_assert(is_task_pool_like_v<task_pool>);
static_assert(is_task_pool_like_v<mutex_task_pool> && is_task_pool_like_v<sharded_task_pool> &&
              is_task_pool_like_v<dispatch_task_pool> && is_task_pool_like_v<mpmc_task_pool> &&
              is_task_pool_like_v<work_stealing_task_pool> && is_task_pool_like_v<inline_task_pool>);
static_assert(is_task_pool_like_v<task_pool_adapter<inline_task_pool>>);
static_assert(!is_task_pool_like_v<int> && !is_task_pool_like_v<worker_count_may_throw>);
#if TS_MOVEABLES_HAS_CONCEPTS
static_assert(task_pool_like<task_pool> && task_pool_like<work_stealing_task_pool> && task_pool_like<inline_task_pool>);
static_assert(!task_pool_like<int> && !task_pool_like<worker_count_may_throw>);
#endif

// A model that is task_pool_like without deriving from task_pool: it runs
// every task inline and counts them, so the forwarding is visible.
struct counting_pool {
    int submitted = 0;
    int waits = 0;
    void submit(task_pool::task t) { ++submitted; t(); }
    void wait_idle() { ++waits; }
    std::size_t worker_count() const noexcept { return 1; }
};
static_assert(is_task_pool_like_v<counting_pool> && !std::is_base_of_v<task_pool, counting_pool>);

void test_task_pool_concept()
{
    // The adapter puts a model behind the interface: by value, and as a view
    task_pool_adapter<counting_pool> owned;
    task_pool& as_iface = owned;
    as_iface.submit([] {});
    as_iface.wait_idle();
    assert(owned.implementation().submitted == 1 && owned.implementation().waits == 1);
    assert(as_iface.worker_count() == 1);

    counting_pool model;
    task_pool_adapter<counting_pool&> view(model);
    static_cast<task_pool&>(view).submit([] {});
    assert(model.submitted == 1);

    // The same algorithm over a concrete pool (direct calls), the interface
    // (virtual calls) and a model that only satisfies the concept
    constexpr int n = 50000;
    work_stealing_task_pool pool{4};
    std::vector<int> a(n), b(n), c(n);
    parallel_for(pool, 0, n, [&](int i) { a[i] = i * 3; });
    parallel_for(static_cast<task_pool&>(pool), 0, n, [&](int i) { b[i] = i * 3; });
    parallel_for(model, 0, n, [&](int i) { c[i] = i * 3; });   // one worker: the serial path
    assert(a == b && b == c);
    const long long want = static_cast<long long>(n) * (n - 1) / 2 * 3;
    auto map = [&](int i) { return static_cast<long long>(a[i]); };
    auto add = [](long long x, long long y) { return x + y; };
    assert(parallel_reduce(pool, 0, n, 0LL, map, add) == want);
    assert(parallel_reduce(static_cast<task_pool&>(pool), 0, n, 0LL, map, add) == want);
    assert(parallel_reduce(model, 0, n, 0LL, map, add) == want);

    // async over a concrete pool and over a model
    auto f = async(pool, [] { return 6 * 7; });
    assert(f.get() == 42);
    auto g = async(model, [] { return std::string("model"); });
    assert(g.get() == "model");

    pass("thread pool: task_pool_like - concrete, interface and adapted pools run the same algorithms");

    // On the concrete pools that have it, submit(F&&) takes the callable as it
    // is: a move-only one, which no std::function can hold, proves the
    // template was chosen and not the submit(task) override
    {
        auto owned_value = std::make_unique<int>(7);
        std::atomic<int> seen{0};
        work_stealing_task_pool ws{4};
        ws.submit([p = std::move(owned_value), &seen] { seen.store(*p); });
        ws.wait_idle();
        assert(seen.load() == 7);

        inline_task_pool in;
        in.submit([p = std::make_unique<int>(9), &seen] { seen.store(*p); });
        assert(seen.load() == 9);

        // Through the interface the same pools still take std::function
        static_cast<task_pool&>(ws).submit([&seen] { seen.store(11); });
        ws.wait_idle();
        assert(seen.load() == 11);
    }
    static_assert(is_pool_task_v<void (*)()> && !is_pool_task_v<int> && !is_pool_task_v<void (*)(int)>);
    pass("thread pool: submit(F&&) on a concrete pool takes a move-only callable as it is");
}

void run_thread_pool_tests()
{
    test_mutex_pool();
    test_sharded_pool();
    test_dispatch_pool();
    test_mpmc_pool();
    test_work_stealing_pool();
    test_work_stealing_forkjoin();
    test_polymorphic_use();
    test_moveability();
    test_reentrant_submit();
    test_task_pool_concept();
}
