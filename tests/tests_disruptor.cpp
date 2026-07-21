//
//  tests_disruptor.cpp
//  TSMoveables
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - unit tests for the disruptor (phase 1)
//

#include "test_helpers.hpp"

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "../TSMoveables/disruptor.hpp"

using namespace snicholls;

namespace {

struct Event {
    std::int64_t value{0};
    std::int64_t doubled{0};    // written by one stage, read by a dependent stage
};

void test_disruptor_basics()
{
    disruptor<Event> d{100};
    assert(d.capacity() == 128);            // rounded up to a power of two
    assert(d.last_published() == -1);

    auto& c = d.add_consumer();

    for (std::int64_t i = 0; i < 10; ++i)
        d.publish([i](Event& e) { e.value = i; });
    assert(d.last_published() == 9);

    // One poll delivers the whole batch, in order, flagged at the end
    std::vector<std::int64_t> seen;
    int batch_ends = 0;
    const auto n = c.poll([&](Event& e, std::int64_t seq, bool end_of_batch) {
        assert(e.value == seq);
        seen.push_back(e.value);
        if (end_of_batch)
            ++batch_ends;
    });
    assert(n == 10);
    assert(batch_ends == 1);
    for (std::int64_t i = 0; i < 10; ++i)
        assert(seen[static_cast<std::size_t>(i)] == i);
    assert(c.last_processed() == 9);
    assert(c.poll([](Event&, std::int64_t, bool) {}) == 0);

    // Zero capacity refuses loudly
    bool threw = false;
    try {
        disruptor<Event> bad{0};
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);

    pass("disruptor basics");
}

void test_disruptor_gating()
{
    // Capacity 4, one consumer that is not polling: the producer must refuse
    // to lap it
    disruptor<Event> d{4};
    auto& c = d.add_consumer();

    for (std::int64_t i = 0; i < 4; ++i)
        assert(d.try_publish([i](Event& e) { e.value = i; }));
    assert(!d.try_publish([](Event& e) { e.value = 99; }));     // full

    assert(c.poll([](Event& e, std::int64_t seq, bool) { assert(e.value == seq); }) == 4);
    assert(d.try_publish([](Event& e) { e.value = 4; }));       // room again

    // Blocking publish completes once the consumer catches up
    std::atomic<bool> published{false};
    std::thread producer([&] {
        for (std::int64_t i = 5; i < 9; ++i)                    // fills, then one blocked publish
            d.publish([i](Event& e) { e.value = i; });
        published = true;
    });
    spin_until([&] { return d.last_published() >= 7; });        // ring is full at 4..7
    assert(!published.load());
    assert(c.poll([](Event&, std::int64_t, bool) {}) > 0);      // frees room
    spin_until([&] { return published.load(); });
    producer.join();
    assert(d.last_published() == 8);

    pass("disruptor gating");
}

void test_disruptor_wiring_after_start_throws()
{
    disruptor<Event> d{8};
    d.add_consumer();
    d.publish([](Event& e) { e.value = 1; });
    bool threw = false;
    try {
        d.add_consumer();
    } catch (const std::logic_error&) {
        threw = true;
    }
    assert(threw);

    pass("disruptor wiring after start throws");
}

void test_disruptor_dependency_graph()
{
    // A and B in parallel; C depends on both. C must never see an event
    // before A and B have processed it - checked live, per event.
    constexpr std::int64_t total = 20000;
    disruptor<Event> d{64};
    auto& a = d.add_consumer();
    auto& b = d.add_consumer();
    auto& c = d.add_consumer({&a, &b});

    std::atomic<bool> keep_running{true};
    std::atomic<bool> barrier_violated{false};
    std::int64_t a_sum = 0, b_sum = 0, c_sum = 0;

    std::thread ta([&] {
        a.run(keep_running, [&](Event& e, std::int64_t, bool) {
            e.doubled = e.value * 2;        // stage A writes for stage C
            a_sum += e.value;
        });
    });
    std::thread tb([&] {
        b.run(keep_running, [&](Event& e, std::int64_t, bool) { b_sum += e.value; });
    });
    std::thread tc([&] {
        c.run(keep_running, [&](Event& e, std::int64_t seq, bool) {
            if (a.last_processed() < seq || b.last_processed() < seq)
                barrier_violated = true;
            if (e.doubled != e.value * 2)   // A's write must be visible
                barrier_violated = true;
            c_sum += e.value;
        });
    });

    for (std::int64_t i = 0; i < total; ++i)
        d.publish([i](Event& e) { e.value = i; e.doubled = 0; });

    spin_until([&] { return c.last_processed() == total - 1; });
    keep_running = false;
    ta.join();
    tb.join();
    tc.join();

    constexpr std::int64_t expected = total * (total - 1) / 2;
    assert(!barrier_violated.load());
    assert(a_sum == expected && b_sum == expected && c_sum == expected);
    assert(a.last_processed() == total - 1);
    assert(b.last_processed() == total - 1);

    pass("disruptor dependency graph");
}

void test_disruptor_batch_publish()
{
    constexpr std::int64_t total = 10000;
    constexpr std::size_t batch = 10;
    disruptor<Event> d{64};
    auto& c = d.add_consumer();

    std::atomic<bool> keep_running{true};
    std::int64_t sum = 0;
    std::thread tc([&] {
        c.run(keep_running, [&](Event& e, std::int64_t seq, bool) {
            assert(e.value == seq);
            sum += e.value;
        });
    });

    for (std::int64_t next = 0; next < total; next += batch)
        d.publish_n(batch, [](Event& e, std::int64_t seq) { e.value = seq; });

    spin_until([&] { return c.last_processed() == total - 1; });
    keep_running = false;
    tc.join();
    assert(sum == total * (total - 1) / 2);

    // A batch larger than the ring cannot be claimed
    bool threw = false;
    try {
        d.publish_n(65, [](Event&, std::int64_t) {});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);

    pass("disruptor batch publish");
}

template <typename Strategy>
void run_strategy_smoke(const char* name)
{
    constexpr std::int64_t total = 5000;
    disruptor<Event, Strategy> d{32};
    auto& c = d.add_consumer();

    std::atomic<bool> keep_running{true};
    std::int64_t sum = 0;
    std::thread tc([&] {
        c.run(keep_running, [&](Event& e, std::int64_t, bool) { sum += e.value; });
    });

    for (std::int64_t i = 0; i < total; ++i)
        d.publish([i](Event& e) { e.value = i; });

    spin_until([&] { return c.last_processed() == total - 1; });
    keep_running = false;
    tc.join();
    assert(sum == total * (total - 1) / 2);

    pass(name);
}

void test_disruptor_move()
{
    // The handle moves freely - even while consumers run - because all shared
    // state lives behind the stable core. References stay valid; publishing
    // continues through the new handle.
    constexpr std::int64_t total = 10000;
    disruptor<Event> d1{64};
    auto& c = d1.add_consumer();

    std::atomic<bool> keep_running{true};
    std::int64_t sum = 0;
    std::thread tc([&] {
        c.run(keep_running, [&](Event& e, std::int64_t, bool) { sum += e.value; });
    });

    for (std::int64_t i = 0; i < total / 2; ++i)
        d1.publish([i](Event& e) { e.value = i; });

    disruptor<Event> d2(std::move(d1));     // mid-flight handle transfer
    for (std::int64_t i = total / 2; i < total; ++i)
        d2.publish([i](Event& e) { e.value = i; });

    spin_until([&] { return c.last_processed() == total - 1; });
    keep_running = false;
    tc.join();
    assert(sum == total * (total - 1) / 2);

    pass("disruptor move (handle transfer mid-flight)");
}

} // namespace

void run_disruptor_tests()
{
    test_disruptor_basics();
    test_disruptor_gating();
    test_disruptor_wiring_after_start_throws();
    test_disruptor_dependency_graph();
    test_disruptor_batch_publish();
    run_strategy_smoke<busy_spin_wait_strategy>("disruptor busy-spin wait strategy");
    run_strategy_smoke<blocking_wait_strategy>("disruptor blocking wait strategy");
    test_disruptor_move();
}
