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
#include "../TSMoveables/concurrent/task_group.hpp"
#include "../TSMoveables/concurrent/thread_pool.hpp"
#include "../TSMoveables/moveable/mutex.hpp"
#include "../TSMoveables/utils/constexpr_for.hpp"

#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <numeric>
#include <random>
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

// ---------------------------------------------------------------- task_group

void test_task_group_runs_all_and_waits()
{
    work_stealing_task_pool pool(4);
    std::atomic<int> n{0};
    {
        task_group g(pool);
        for (int i = 0; i < 200; ++i)
            g.run([&] { n.fetch_add(1); });
        g.wait();
        assert(n.load() == 200);
    }
    // And the destructor waits too, so forgetting wait() stalls rather than
    // letting tasks touch a dead frame.
    std::atomic<int> m{0};
    {
        task_group g(pool);
        for (int i = 0; i < 128; ++i)
            g.run([&] { m.fetch_add(1); });
    }
    assert(m.load() == 128);

    pass("task_group: runs every task, and the destructor waits");
}

void test_task_group_propagates_the_first_exception()
{
    work_stealing_task_pool pool(4);
    std::atomic<int> ran{0};
    bool caught = false;
    try {
        task_group g(pool);
        g.run([&] { throw std::runtime_error("boom"); });
        for (int i = 0; i < 64; ++i)
            g.run([&] { ran.fetch_add(1); });
        g.wait();
    } catch (const std::runtime_error& e) {
        caught = std::string(e.what()) == "boom";
    }
    assert(caught);
    pass("task_group: the first exception reaches wait(), and it does not hang");
}

void test_task_group_nests_without_deadlock()
{
    // The case the waiting thread running tasks exists for: every worker is
    // parked in an inner wait(), so if waiting were passive there would be no
    // thread left to run what they are waiting for. Two workers, eight outer
    // tasks - guaranteed to occupy them all.
    work_stealing_task_pool pool(2);
    std::atomic<int> n{0};
    task_group outer(pool);
    for (int i = 0; i < 8; ++i)
        outer.run([&] {
            task_group inner(pool);
            for (int k = 0; k < 32; ++k)
                inner.run([&] { n.fetch_add(1); });
            inner.wait();
        });
    outer.wait();
    assert(n.load() == 8 * 32);
    pass("task_group: nested groups complete rather than deadlock");
}

// ------------------------------------------------------------------- reduce

void test_parallel_reduce_matches_serial()
{
    work_stealing_task_pool pool;
    const int n = 100000;
    std::vector<double> v(n);
    for (int i = 0; i < n; ++i)
        v[i] = double((i % 97) + 1);

    const double got = parallel_reduce(pool, 0, n, 0.0,
                                       [&](int i) { return v[i]; }, std::plus<double>{});
    const double want = std::accumulate(v.begin(), v.end(), 0.0);
    assert(got == want);                    // exact: these are small integers in doubles

    // Empty and single ranges.
    assert(parallel_reduce(pool, 0, 0, 7.0, [&](int) { return 1.0; }, std::plus<double>{}) == 7.0);
    assert(parallel_reduce(pool, 3, 4, 0.0, [&](int i) { return double(i); }, std::plus<double>{}) == 3.0);

    pass("parallel_reduce: matches the serial fold, and handles empty ranges");
}

void test_parallel_reduce_is_deterministic()
{
    // The property the chunk-ordered fold buys. Values chosen so the sum is
    // genuinely order-sensitive in floating point: without the ordering this
    // varies run to run.
    work_stealing_task_pool pool;
    const int n = 200000;
    std::vector<double> v(n);
    for (int i = 0; i < n; ++i)
        v[i] = 1.0 / double(i + 1);

    const double first = parallel_reduce(pool, 0, n, 0.0,
                                         [&](int i) { return v[i]; }, std::plus<double>{});
    for (int r = 0; r < 50; ++r) {
        const double again = parallel_reduce(pool, 0, n, 0.0,
                                             [&](int i) { return v[i]; }, std::plus<double>{});
        assert(again == first);             // bit for bit, whatever the scheduler did
    }

    pass("parallel_reduce: bit-identical across 50 runs, not completion-ordered");
}

void test_parallel_reduce_keeps_index_order()
{
    // Associativity is required; commutativity is NOT, because the fold runs in
    // chunk order. String concatenation would scramble under a
    // completion-ordered reduction.
    work_stealing_task_pool pool;
    std::vector<std::string> w;
    for (char c = 'a'; c <= 'z'; ++c)
        w.push_back(std::string(1, c));

    const std::string got = parallel_reduce_each(
        pool, w.begin(), w.end(), std::string{},
        [](const std::string& s) { return s; },
        [](std::string a, std::string b) { return a + b; }, 1);
    assert(got == "abcdefghijklmnopqrstuvwxyz");

    pass("parallel_reduce: folds in index order, so non-commutative ops are safe");
}

void test_parallel_reduce_propagates_exceptions()
{
    work_stealing_task_pool pool;
    bool caught = false;
    try {
        parallel_reduce(pool, 0, 4096, 0,
                        [&](int i) -> int { if (i == 2000) throw std::runtime_error("boom"); return 1; },
                        std::plus<int>{});
    } catch (const std::runtime_error& e) {
        caught = std::string(e.what()) == "boom";
    }
    assert(caught);
    pass("parallel_reduce: a throwing map propagates and does not hang");
}

// --------------------------------------------------------------------- scan

void test_parallel_scan_matches_std()
{
    work_stealing_task_pool pool;
    for (int n : {0, 1, 2, 3, 17, 1000, 100000}) {
        const std::size_t sz = static_cast<std::size_t>(n);
        std::vector<double> v(sz);
        for (int i = 0; i < n; ++i)
            v[static_cast<std::size_t>(i)] = double((i % 7) + 1);

        std::vector<double> got(sz, -1.0), want(sz, -1.0);
        const double total =
            parallel_inclusive_scan(pool, v.begin(), v.end(), got.begin(), 0.0, std::plus<double>{});
        std::inclusive_scan(v.begin(), v.end(), want.begin(), std::plus<double>{}, 0.0);
        assert(got == want);
        if (n)
            assert(total == want.back());

        std::vector<double> gotx(sz, -1.0), wantx(sz, -1.0);
        parallel_exclusive_scan(pool, v.begin(), v.end(), gotx.begin(), 0.0, std::plus<double>{});
        if (n)
            std::exclusive_scan(v.begin(), v.end(), wantx.begin(), 0.0, std::plus<double>{});
        assert(gotx == wantx);
    }
    pass("parallel_scan: inclusive and exclusive match std, across chunk boundaries");
}

// --------------------------------------------------------------------- sort

void test_parallel_sort_matches_std()
{
    work_stealing_task_pool pool;
    std::mt19937 rng(1234);
    for (int n : {0, 1, 2, 3, 4097, 200000}) {
        const std::size_t sz = static_cast<std::size_t>(n);
        std::vector<int> v(sz);
        for (int i = 0; i < n; ++i)
            v[static_cast<std::size_t>(i)] = int(rng() % 1000);          // duplicates on purpose
        std::vector<int> want = v;

        parallel_sort(pool, v.begin(), v.end());
        std::sort(want.begin(), want.end());
        assert(v == want);
    }

    // A comparator other than less<>.
    std::vector<int> g(50000);
    for (std::size_t i = 0; i < g.size(); ++i)
        g[i] = int(rng() % 100);
    std::vector<int> gw = g;
    parallel_sort(pool, g.begin(), g.end(), std::greater<>{});
    std::sort(gw.begin(), gw.end(), std::greater<>{});
    assert(g == gw);

    pass("parallel_sort: matches std::sort, with duplicates and a comparator");
}

void test_parallel_stable_sort_is_stable()
{
    work_stealing_task_pool pool;
    struct item { int key; int id; };
    std::vector<item> v(40000);
    for (std::size_t i = 0; i < v.size(); ++i) {
        v[i].key = int(i % 13);                             // many equal keys
        v[i].id  = int(i);
    }
    parallel_stable_sort(pool, v.begin(), v.end(),
                         [](const item& a, const item& b) { return a.key < b.key; });

    for (std::size_t i = 1; i < v.size(); ++i) {
        assert(v[i - 1].key <= v[i].key);
        if (v[i - 1].key == v[i].key)
            assert(v[i - 1].id < v[i].id);                  // equal keys kept their order
    }
    pass("parallel_stable_sort: equal keys keep their original order");
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
    test_task_group_runs_all_and_waits();
    test_task_group_propagates_the_first_exception();
    test_task_group_nests_without_deadlock();
    test_parallel_reduce_matches_serial();
    test_parallel_reduce_is_deterministic();
    test_parallel_reduce_keeps_index_order();
    test_parallel_reduce_propagates_exceptions();
    test_parallel_scan_matches_std();
    test_parallel_sort_matches_std();
    test_parallel_stable_sort_is_stable();
}
