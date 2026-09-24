//
//  tests_parallel_for.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - unit tests for the data-parallel loops
//
//  The claims under test: every element is visited exactly once, across every
//  pool implementation; the work actually spreads across workers; an uneven
//  body still balances (which is what chunk-claiming buys over fixed slices);
//  a body that throws propagates the first exception and does not hang; the
//  call is safe to nest inside a pool task (the deadlock the calling thread
//  participating is there to prevent); and the serial fallbacks are taken.
//

#include "test_helpers.hpp"

#include "../TSMoveables/concurrent/parallel_for.hpp"
#include "../TSMoveables/concurrent/thread_pool.hpp"
#include "../TSMoveables/moveable/mutex.hpp"
#include "../TSMoveables/utils/constexpr_for.hpp"

#include <atomic>
#include <map>
#include <mutex>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using namespace snicholls;

namespace {

// Every element exactly once, on each pool in turn - the loop binds to the
// task_pool interface, so "it works" has to mean "on all of them".
template <typename Pool>
void each_element_exactly_once(const char* which)
{
    Pool pool;
    const int n = 10000;
    std::vector<std::atomic<int>> hits(n);
    for (auto& h : hits)
        h.store(0);

    parallel_for(pool, 0, n, [&](int i) { hits[i].fetch_add(1); });

    for (int i = 0; i < n; ++i)
        assert(hits[i].load() == 1 && which);
}

void test_parallel_for_visits_every_element_once_on_every_pool()
{
    each_element_exactly_once<mutex_task_pool>("mutex");
    each_element_exactly_once<sharded_task_pool>("sharded");
    each_element_exactly_once<mpmc_task_pool>("mpmc");
    each_element_exactly_once<work_stealing_task_pool>("work_stealing");

    pass("parallel_for: every element runs exactly once, on all four pools");
}

// Force at least two threads into the loop body rather than hoping the
// scheduler provides them.
//
// The first arrival blocks until a second shows up, so a single thread CANNOT
// drain the whole range: it is parked inside the body, and the remaining
// chunks are there for anyone else to claim. That turns "did it spread?" from
// a timing observation into something the test makes true - or fails on.
//
// Bounded by a deadline so a starved runner can never hang CI; if the gate
// times out the design under test did not do what it claims, which is a
// failure rather than a reason to skip.
class two_thread_gate {
public:
    bool arrive()
    {
        if (arrived_.fetch_add(1) + 1 >= 2)
            return true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (arrived_.load() < 2) {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::yield();
        }
        return true;
    }
    bool opened() const { return arrived_.load() >= 2; }

private:
    std::atomic<int> arrived_{0};
};

void test_parallel_for_actually_uses_more_than_one_thread()
{
    // Correctness above is satisfied by a serial loop, so it cannot tell
    // whether anything ran in parallel at all. This can - but only because the
    // gate makes a single-threaded run impossible rather than unlikely.
    //
    // An explicit two-worker pool, not hardware_concurrency: the behaviour
    // under test must be the same on a 2-core CI runner as on a 32-thread
    // workstation, and depending on the host's core count is how the first
    // version of the test below came to pass locally and fail on Windows.
    work_stealing_task_pool pool(2);

    two_thread_gate gate;
    moveable_mutex<> mtx;
    std::set<std::thread::id> threads;
    parallel_for(pool, 0, 512, [&](int) {
        gate.arrive();
        std::lock_guard<moveable_mutex<>> g(mtx);
        threads.insert(std::this_thread::get_id());
    }, 16);                                      // 32 chunks, so there is always more to claim

    assert(gate.opened());                       // a second thread really did arrive
    assert(threads.size() > 1);
    pass("parallel_for: the range really is spread across threads");
}

void test_parallel_for_balances_an_uneven_body()
{
    // The reason chunks are claimed from a cursor rather than handed out as
    // fixed slices. All the expensive elements are at one end, so a fixed-slice
    // split hands them to one worker while the others idle; with claiming, the
    // threads that drew cheap chunks come back for more.
    //
    // Two things make this deterministic rather than a property of the host,
    // and the first version of this test had neither - it passed on a 32-thread
    // machine and failed on every 2-core CI runner:
    //
    //   grain is EXPLICIT. The default is ceil(n / (workers*4)), so on two
    //   workers it would be 512 and all 256 expensive elements would sit in
    //   chunk 0 - one thread doing all of them, correctly, with nothing for
    //   anyone to steal. That is not an imbalance the design claims to fix; it
    //   is a range that was never divided. At grain 16 the expensive prefix
    //   spans 16 chunks and there is something to spread.
    //
    //   the gate forces a second thread in. Otherwise the calling thread can
    //   legitimately drain the whole range before a worker is scheduled, which
    //   is also correct behaviour and would make the assertion a coin flip.
    work_stealing_task_pool pool(2);

    const int n = 4096;
    const int expensive_below = 256;            // the costly elements, all at the front
    two_thread_gate gate;
    moveable_mutex<> mtx;
    std::map<std::thread::id, int> expensive_per_thread;

    parallel_for(pool, 0, n, [&](int i) {
        if (i < expensive_below) {
            gate.arrive();                      // only the first arrival waits
            volatile double sink = 0;           // enough work to matter, no sleeping
            for (int k = 0; k < 20000; ++k)
                sink += k * 0.5;
            std::lock_guard<moveable_mutex<>> g(mtx);
            ++expensive_per_thread[std::this_thread::get_id()];
        }
    }, 16);

    int total = 0, worst = 0;
    for (const auto& e : expensive_per_thread) {
        total += e.second;
        worst = (e.second > worst) ? e.second : worst;
    }
    assert(total == expensive_below);           // every expensive element ran once

    // The gate opening IS the design claim: with the expensive work spread over
    // 16 chunks and one thread parked in the first of them, a second thread had
    // to be able to claim another. A fixed-slice implementation would have given
    // the whole prefix to one worker, which would then wait at the gate until
    // the deadline - so this assertion fails rather than skips.
    assert(gate.opened());
    assert(expensive_per_thread.size() > 1);
    assert(worst < expensive_below);

    pass("parallel_for: an uneven body still spreads, not one slice per worker");
}

void test_parallel_for_propagates_the_first_exception()
{
    work_stealing_task_pool pool;
    std::atomic<int> ran{0};
    bool caught = false;
    try {
        parallel_for(pool, 0, 4096, [&](int i) {
            ran.fetch_add(1);
            if (i == 1000)
                throw std::runtime_error("boom");
        });
    } catch (const std::runtime_error& e) {
        caught = true;
        assert(std::string(e.what()) == "boom");
    }
    assert(caught);
    // And it returned rather than hanging, which is the half that a naive
    // "stop claiming on failure" implementation gets wrong: the unclaimed
    // chunks never decrement the gate and the caller waits forever.
    assert(ran.load() > 0);

    pass("parallel_for: a throwing body propagates and does not hang");
}

void test_parallel_for_nests_without_deadlock()
{
    // The case the calling thread participating exists for. An outer loop whose
    // body runs another parallel_for occupies every worker waiting on inner
    // work. If the caller only waited, there would be no thread left to run
    // what it is waiting for. Because the caller drains chunks itself, the
    // worst case is serial rather than stopped.
    //
    // A pool deliberately smaller than the outer range, so the outer loop is
    // guaranteed to occupy every worker at once.
    work_stealing_task_pool pool(2);
    const int outer = 8, inner = 64;
    std::atomic<int> total{0};

    parallel_for(pool, 0, outer, [&](int) {
        parallel_for(pool, 0, inner, [&](int) { total.fetch_add(1); });
    });

    assert(total.load() == outer * inner);
    pass("parallel_for: nested inside a pool task, it completes rather than deadlocks");
}

void test_parallel_for_serial_fallbacks()
{
    work_stealing_task_pool pool;

    // An empty or inverted range does nothing at all.
    int calls = 0;
    parallel_for(pool, 5, 5, [&](int) { ++calls; });
    parallel_for(pool, 9, 3, [&](int) { ++calls; });
    assert(calls == 0);

    // A single element still runs, and on this thread - the serial path must
    // not skip work, only skip the machinery.
    const std::thread::id here = std::this_thread::get_id();
    std::thread::id ran_on{};
    parallel_for(pool, 7, 8, [&](int i) {
        assert(i == 7);
        ran_on = std::this_thread::get_id();
    });
    assert(ran_on == here);

    pass("parallel_for: empty, inverted and single-element ranges take the serial path");
}

void test_parallel_for_respects_an_explicit_grain()
{
    // One chunk per element, and one chunk for everything, must both be exact.
    work_stealing_task_pool pool;
    const int n = 1000;

    std::vector<std::atomic<int>> fine(n), coarse(n);
    for (int i = 0; i < n; ++i) { fine[i].store(0); coarse[i].store(0); }

    parallel_for(pool, 0, n, [&](int i) { fine[i].fetch_add(1); }, 1);
    parallel_for(pool, 0, n, [&](int i) { coarse[i].fetch_add(1); }, n);

    for (int i = 0; i < n; ++i) {
        assert(fine[i].load() == 1);
        assert(coarse[i].load() == 1);
    }

    pass("parallel_for: grain of 1 and grain of n both cover the range exactly");
}

void test_parallel_for_each_over_a_container()
{
    work_stealing_task_pool pool;
    std::vector<int> v(5000);
    std::iota(v.begin(), v.end(), 0);
    const long long expected =
        std::accumulate(v.begin(), v.end(), 0LL, [](long long a, int b) { return a + b * 2LL; });

    parallel_for_each(pool, v.begin(), v.end(), [](int& x) { x *= 2; });

    const long long got = std::accumulate(v.begin(), v.end(), 0LL);
    assert(got == expected);

    // Empty range is a no-op rather than an error.
    std::vector<int> empty;
    parallel_for_each(pool, empty.begin(), empty.end(), [](int&) { assert(false); });

    pass("parallel_for_each: mutates every element of a container, empty is a no-op");
}

// ------------------------------------------------- compile-time unrolling

void test_constexpr_for_expands_the_range()
{
    int sum = 0;
    constexpr_for<0, 4>([&](auto I) { sum += I.value; });
    assert(sum == 0 + 1 + 2 + 3);

    sum = 0;
    constexpr_for<4>([&](auto I) { sum += I.value; });       // [0, Count) spelling
    assert(sum == 0 + 1 + 2 + 3);

    // An empty range generates nothing at all rather than one stray call.
    int calls = 0;
    constexpr_for<3, 3>([&](auto) { ++calls; });
    assert(calls == 0);

    pass("constexpr_for: expands [Start, End), and an empty range expands to nothing");
}

void test_constexpr_for_steps_and_counts_down()
{
    int sum = 0;
    constexpr_for<0, 10, 2>([&](auto I) { sum += I; });      // 0 2 4 6 8
    assert(sum == 20);

    std::vector<int> order;
    constexpr_for<5, 0, -1>([&](auto I) { order.push_back(I); });
    assert((order == std::vector<int>{5, 4, 3, 2, 1}));      // 0 is the open end

    // A step that overshoots still runs exactly once.
    int once = 0;
    constexpr_for<0, 3, 10>([&](auto) { ++once; });
    assert(once == 1);

    pass("constexpr_for: honours a step, including a negative one");
}

void test_constexpr_for_index_is_a_compile_time_constant()
{
    // The reason the index is an integral_constant rather than a size_t: a
    // function parameter is never constexpr, so this would not compile if the
    // body were handed a plain value.
    auto tup = std::make_tuple(1, 2.5, 3);
    double total = 0;
    constexpr_for<0, 3>([&](auto I) { total += static_cast<double>(std::get<I.value>(tup)); });
    assert(total == 6.5);

    pass("constexpr_for: the index is usable as a template argument");
}

void test_constexpr_for_forwards_extra_arguments()
{
    int counter = 0;
    // Passed as lvalues to every iteration - the body sees the same object,
    // so a move would be a use-after-move on the second pass.
    constexpr_for<0, 3>([](auto, int& c) { c += 2; }, counter);
    assert(counter == 6);

    pass("constexpr_for: trailing arguments reach every iteration as lvalues");
}

void test_constexpr_nest_unrolls_nested_loops()
{
    std::vector<std::pair<int, int>> visited;
    constexpr_nest<2, 3>([&](auto I, auto J) {
        visited.emplace_back(static_cast<int>(I.value), static_cast<int>(J.value));
    });
    const std::vector<std::pair<int, int>> expect_2d{{0,0},{0,1},{0,2},{1,0},{1,1},{1,2}};
    assert(visited == expect_2d);                            // row-major, last varies fastest

    int count3 = 0, count4 = 0;
    constexpr_nest<2, 3, 4>([&](auto, auto, auto) { ++count3; });
    constexpr_nest<2, 2, 2, 2>([&](auto, auto, auto, auto) { ++count4; });
    assert(count3 == 2 * 3 * 4);
    assert(count4 == 16);

    // Indices stay compile-time constants at every level.
    int fixed = 0;
    constexpr_nest<2, 2>([&](auto I, auto J) {
        constexpr std::size_t flat = I.value * 2 + J.value;
        fixed += static_cast<int>(flat);
    });
    assert(fixed == 0 + 1 + 2 + 3);

    pass("constexpr_nest: unrolls 2, 3 and 4 deep with constant indices throughout");
}

void test_unrolled_for_covers_range_and_tail()
{
    // The tail is where a hand-rolled unroll goes wrong: 10 elements by 4 is
    // two whole passes and a remainder of 2.
    for (std::size_t n = 0; n < 40; ++n) {
        std::vector<int> hits(n, 0);
        unrolled_for<4>(std::size_t{0}, n, [&](std::size_t i) { ++hits[i]; });
        for (std::size_t i = 0; i < n; ++i)
            assert(hits[i] == 1);
    }

    // Unroll of 1 is a plain loop, and an inverted range does nothing.
    int calls = 0;
    unrolled_for<1>(0, 5, [&](int) { ++calls; });
    assert(calls == 5);
    unrolled_for<4>(9, 3, [&](int) { assert(false); });

    pass("unrolled_for: every element once for every length, tail included");
}

void test_parallel_for_unrolled_matches_plain()
{
    work_stealing_task_pool pool;
    const int n = 5000;
    std::vector<int> plain(n, 0), rolled(n, 0);

    parallel_for(pool, 0, n, [&](int i) { plain[i] = i * 3; });
    parallel_for<8>(pool, 0, n, [&](int i) { rolled[i] = i * 3; });

    assert(plain == rolled);
    pass("parallel_for<Unroll>: same result as the plain loop");
}

} // namespace

void run_parallel_for_tests()
{
    test_parallel_for_visits_every_element_once_on_every_pool();
    test_parallel_for_actually_uses_more_than_one_thread();
    test_parallel_for_balances_an_uneven_body();
    test_parallel_for_propagates_the_first_exception();
    test_parallel_for_nests_without_deadlock();
    test_parallel_for_serial_fallbacks();
    test_parallel_for_respects_an_explicit_grain();
    test_parallel_for_each_over_a_container();
    test_constexpr_for_expands_the_range();
    test_constexpr_for_steps_and_counts_down();
    test_constexpr_for_index_is_a_compile_time_constant();
    test_constexpr_for_forwards_extra_arguments();
    test_constexpr_nest_unrolls_nested_loops();
    test_unrolled_for_covers_range_and_tail();
    test_parallel_for_unrolled_matches_plain();
}
