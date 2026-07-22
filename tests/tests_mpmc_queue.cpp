//
//  tests_mpmc_queue.cpp
//  TSMoveables
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - unit tests for the bounded MPMC queue
//

#include "test_helpers.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "../TSMoveables/mpmc_queue.hpp"

using namespace snicholls;

namespace {

struct Counted {
    static inline std::atomic<int> alive{0};
    int v{0};
    explicit Counted(int x = 0) : v(x) { ++alive; }
    Counted(const Counted& o) : v(o.v) { ++alive; }
    Counted(Counted&& o) noexcept : v(o.v) { ++alive; }
    Counted& operator=(const Counted&) = default;
    Counted& operator=(Counted&&) = default;
    ~Counted() { --alive; }
};

void test_mpmc_basics()
{
    mpmc_queue<int> q{6};
    assert(q.capacity() == 8);              // rounded up to a power of two
    assert(q.empty());

    for (int i = 0; i < 8; ++i)
        assert(q.push(i));
    assert(!q.push(99));                    // full
    assert(q.size() == 8);

    int out = 0;
    for (int i = 0; i < 8; ++i) {
        assert(q.try_pop(out));
        assert(out == i);                   // FIFO
    }
    assert(!q.try_pop(out));                // empty
    assert(!q.try_pop().has_value());

    // optional pop and emplace
    mpmc_queue<std::pair<int, int>> pq{4};
    assert(pq.emplace(1, 2));
    auto v = pq.try_pop();
    assert(v && v->first == 1 && v->second == 2);

    pass("mpmc queue basics");
}

void test_mpmc_wraparound()
{
    mpmc_queue<int> q{4};
    int out = 0;
    for (int cycle = 0; cycle < 100000; ++cycle) {
        assert(q.push(cycle));
        assert(q.try_pop(out));
        assert(out == cycle);
    }
    assert(q.empty());
    pass("mpmc queue wrap-around");
}

void test_mpmc_lifetimes()
{
    {
        mpmc_queue<Counted> q{8};
        for (int i = 0; i < 6; ++i)
            assert(q.emplace(i));
        Counted out;
        assert(q.try_pop(out) && out.v == 0);
        assert(q.try_pop(out) && out.v == 1);
    }                                       // 4 remain - destructor drains them
    assert(Counted::alive.load() == 0);

    // Move-only element type
    mpmc_queue<std::unique_ptr<int>> uq{4};
    assert(uq.push(std::make_unique<int>(7)));
    auto p = uq.try_pop();
    assert(p && **p == 7);

    pass("mpmc queue element lifetimes");
}

void test_mpmc_move()
{
    mpmc_queue<int> a{8};
    for (int i = 0; i < 5; ++i)
        a.push(i);
    mpmc_queue<int> b(std::move(a));        // quiescent move
    assert(b.size() == 5);
    int out = 0;
    for (int i = 0; i < 5; ++i) {
        assert(b.try_pop(out));
        assert(out == i);
    }
    mpmc_queue<int> c{2};
    c = std::move(b);
    assert(c.empty());
    assert(c.push(1));

    pass("mpmc queue move");
}

// The real test: many producers and many consumers, every value delivered
// exactly once with none lost or duplicated
void test_mpmc_concurrent()
{
    constexpr int n_producers = 4;
    constexpr int n_consumers = 4;
    constexpr int per_producer = 100000;
    constexpr long long total = static_cast<long long>(n_producers) * per_producer;

    mpmc_queue<std::int64_t> q{1024};
    std::atomic<long long> produced{0};
    std::atomic<long long> consumed{0};
    std::atomic<long long> sum{0};
    std::atomic<bool> producers_done{false};

    std::vector<std::thread> threads;
    for (int p = 0; p < n_producers; ++p)
        threads.emplace_back([&, p] {
            for (int i = 0; i < per_producer; ++i) {
                // Encode producer and index so the consumer can verify uniqueness
                const std::int64_t value = static_cast<std::int64_t>(p) * per_producer + i;
                while (!q.push(value))
                    std::this_thread::yield();      // backpressure when full
                produced.fetch_add(1, std::memory_order_relaxed);
            }
        });

    for (int cns = 0; cns < n_consumers; ++cns)
        threads.emplace_back([&] {
            std::int64_t v;
            for (;;) {
                if (q.try_pop(v)) {
                    sum.fetch_add(v, std::memory_order_relaxed);
                    if (consumed.fetch_add(1, std::memory_order_acq_rel) + 1 == total)
                        return;
                } else if (producers_done.load(std::memory_order_acquire) &&
                           consumed.load(std::memory_order_acquire) >= total) {
                    return;
                } else {
                    std::this_thread::yield();
                }
            }
        });

    // Wait for producers, then let consumers finish
    for (int p = 0; p < n_producers; ++p)
        threads[static_cast<std::size_t>(p)].join();
    producers_done.store(true, std::memory_order_release);
    for (std::size_t i = n_producers; i < threads.size(); ++i)
        threads[i].join();

    assert(produced.load() == total);
    assert(consumed.load() == total);
    // sum of 0..total-1 - proves every value delivered exactly once
    assert(sum.load() == total * (total - 1) / 2);

    pass("mpmc queue concurrent (4 producers, 4 consumers, exactly-once)");
}

} // namespace

void run_mpmc_queue_tests()
{
    test_mpmc_basics();
    test_mpmc_wraparound();
    test_mpmc_lifetimes();
    test_mpmc_move();
    test_mpmc_concurrent();
}
