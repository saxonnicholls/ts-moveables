//
//  tests_disruptor.cpp
//  TSMoveables
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - unit tests for the disruptor
//  (phase 1: single producer; phase 2: multi-producer)
//

#include "test_helpers.hpp"

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <thread>
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

// ------------------------------------------- phase 2: multiple producers ---

// Every multi-producer test below hands each producer a disjoint block of
// values, so "exactly once" is checkable by tallying the values the consumer
// saw. A lost event leaves a zero, a duplicated one leaves a two.
struct tally {
    std::vector<char> seen;

    explicit tally(std::int64_t total) : seen(static_cast<std::size_t>(total), 0) {}

    void record(std::int64_t value) {
        assert(value >= 0 && value < static_cast<std::int64_t>(seen.size()));
        ++seen[static_cast<std::size_t>(value)];
    }

    void check_exactly_once() const {
        for (std::size_t i = 0; i < seen.size(); ++i)
            assert(seen[i] == 1);
    }
};

void test_disruptor_multi_producer_exactly_once()
{
    // Four producers into a ring far smaller than the traffic, so every slot
    // is recycled hundreds of times. Two publish singly, two in batches, to
    // exercise both claim paths against each other.
    constexpr int producers = 4;
    constexpr std::int64_t per_producer = 10000;
    constexpr std::size_t batch = 8;
    constexpr std::int64_t total = producers * per_producer;

    multi_producer_disruptor<Event> d{64};
    auto& c = d.add_consumer();

    tally t{total};
    std::atomic<bool> keep_running{true};
    std::atomic<std::int64_t> received{0};
    std::atomic<bool> out_of_order{false};
    std::int64_t expect_seq = 0;                // consumer thread only

    std::thread tc([&] {
        c.run(keep_running, [&](Event& e, std::int64_t seq, bool) {
            // Whatever order the producers publish in, the consumer must see a
            // dense ascending stream - that is what the availability marks buy
            if (seq != expect_seq)
                out_of_order = true;
            ++expect_seq;
            t.record(e.value);
            received.fetch_add(1, std::memory_order_release);
        });
    });

    std::vector<std::thread> tp;
    for (int p = 0; p < producers; ++p)
        tp.emplace_back([&, p] {
            const std::int64_t base = p * per_producer;
            if (p % 2 == 0) {
                for (std::int64_t i = 0; i < per_producer; ++i)
                    d.publish([v = base + i](Event& e) { e.value = v; });
            } else {
                for (std::int64_t i = 0; i < per_producer;
                     i += static_cast<std::int64_t>(batch)) {
                    std::int64_t v = base + i;
                    d.publish_n(batch, [&v](Event& e, std::int64_t) { e.value = v++; });
                }
            }
        });
    for (auto& p : tp)
        p.join();

    assert(spin_until_for([&] { return received.load(std::memory_order_acquire) == total; }));
    keep_running = false;
    tc.join();

    assert(!out_of_order.load());
    t.check_exactly_once();
    assert(d.last_published() == total - 1);
    assert(c.last_processed() == total - 1);

    pass("disruptor multi-producer exactly-once delivery");
}

void test_disruptor_multi_producer_out_of_order_publication()
{
    // The heart of phase 2. Producer A claims sequence 0 and stops inside its
    // fill; producer B then claims and publishes sequence 1. The consumer must
    // see neither - stepping over the hole would hand it a half-written event.
    multi_producer_disruptor<Event> d{8};
    auto& c = d.add_consumer();

    std::atomic<bool> a_claimed{false};
    std::atomic<bool> release_a{false};

    std::thread a([&] {
        d.publish([&](Event& e) {
            e.value = 100;
            a_claimed.store(true, std::memory_order_release);
            while (!release_a.load(std::memory_order_acquire))
                std::this_thread::yield();
        });
    });
    // A is inside fill, so it has already claimed 0 and B can only get 1
    assert(spin_until_for([&] { return a_claimed.load(std::memory_order_acquire); }));

    std::thread b([&] { d.publish([](Event& e) { e.value = 200; }); });
    b.join();                                   // 1 is fully published

    assert(c.poll([](Event&, std::int64_t, bool) { assert(false); }) == 0);
    assert(d.last_published() == -1);           // the hole at 0 gates everything

    release_a.store(true, std::memory_order_release);
    a.join();

    std::vector<std::int64_t> seen;
    const auto n = c.poll([&](Event& e, std::int64_t, bool) { seen.push_back(e.value); });
    assert(n == 2);
    assert(seen.size() == 2 && seen[0] == 100 && seen[1] == 200);
    assert(d.last_published() == 1);

    pass("disruptor multi-producer out-of-order publication is gated");
}

void test_disruptor_multi_producer_gating()
{
    // Capacity 4, one consumer that is not polling: the CAS claim must refuse
    // to lap it, and a blocking publish must genuinely block.
    multi_producer_disruptor<Event> d{4};
    auto& c = d.add_consumer();

    for (std::int64_t i = 0; i < 4; ++i)
        assert(d.try_publish([i](Event& e) { e.value = i; }));
    assert(!d.try_publish([](Event& e) { e.value = 99; }));     // full

    assert(c.poll([](Event& e, std::int64_t seq, bool) { assert(e.value == seq); }) == 4);
    assert(d.try_publish([](Event& e) { e.value = 4; }));       // room again

    std::atomic<bool> published{false};
    std::thread producer([&] {
        for (std::int64_t i = 5; i < 9; ++i)                    // fills, then one blocked publish
            d.publish([i](Event& e) { e.value = i; });
        published = true;
    });
    assert(spin_until_for([&] { return d.last_published() >= 7; }));    // ring full at 4..7
    assert(!published.load());
    assert(c.poll([](Event&, std::int64_t, bool) {}) > 0);      // frees room
    assert(spin_until_for([&] { return published.load(); }));
    producer.join();
    assert(d.last_published() == 8);

    // A batch larger than the ring cannot be claimed here either
    // (std::invalid_argument is a std::logic_error)
    assert(throws_logic([&] { d.publish_n(5, [](Event&, std::int64_t) {}); }));

    pass("disruptor multi-producer gating");
}

void test_disruptor_multi_producer_wraparound()
{
    // Six producers through a ring of four: every slot is claimed, published
    // and reclaimed thousands of times. A stale availability mark - the thing
    // the round number exists to prevent - would surface here immediately as a
    // duplicated or lost event, or as a consumer reading a half-written slot.
    constexpr int producers = 6;
    constexpr std::int64_t per_producer = 3000;
    constexpr std::int64_t total = producers * per_producer;

    multi_producer_disruptor<Event> d{4};
    auto& c = d.add_consumer();

    tally t{total};
    std::atomic<bool> keep_running{true};
    std::atomic<std::int64_t> received{0};
    std::atomic<bool> torn{false};

    std::thread tc([&] {
        c.run(keep_running, [&](Event& e, std::int64_t, bool) {
            // doubled is written by the producer alongside value; seeing them
            // disagree means the slot was read before its producer finished
            if (e.doubled != e.value * 2)
                torn = true;
            t.record(e.value);
            received.fetch_add(1, std::memory_order_release);
        });
    });

    std::vector<std::thread> tp;
    for (int p = 0; p < producers; ++p)
        tp.emplace_back([&, p] {
            const std::int64_t base = p * per_producer;
            for (std::int64_t i = 0; i < per_producer; ++i)
                d.publish([v = base + i](Event& e) {
                    e.value = v;
                    e.doubled = v * 2;
                });
        });
    for (auto& p : tp)
        p.join();

    assert(spin_until_for([&] { return received.load(std::memory_order_acquire) == total; }));
    keep_running = false;
    tc.join();

    assert(!torn.load());
    t.check_exactly_once();
    assert(c.last_processed() == total - 1);

    pass("disruptor multi-producer wraparound (availability marks)");
}

void test_disruptor_multi_producer_dependency_graph()
{
    // A and B in parallel, C behind both, fed by three producers. A dependent
    // consumer's barrier is its dependencies rather than the availability
    // marks, so this is a different code path from the tests above.
    constexpr int producers = 3;
    constexpr std::int64_t per_producer = 4000;
    constexpr std::int64_t total = producers * per_producer;

    multi_producer_disruptor<Event> d{64};
    auto& a = d.add_consumer();
    auto& b = d.add_consumer();
    auto& c = d.add_consumer({&a, &b});

    std::atomic<bool> keep_running{true};
    std::atomic<bool> barrier_violated{false};
    std::atomic<std::int64_t> c_received{0};
    tally t{total};

    std::thread ta([&] {
        a.run(keep_running, [&](Event& e, std::int64_t, bool) {
            e.doubled = e.value * 2;        // stage A writes for stage C
        });
    });
    std::thread tb([&] {
        b.run(keep_running, [&](Event&, std::int64_t, bool) {});
    });
    std::thread tc([&] {
        c.run(keep_running, [&](Event& e, std::int64_t seq, bool) {
            if (a.last_processed() < seq || b.last_processed() < seq)
                barrier_violated = true;
            if (e.doubled != e.value * 2)   // A's write must be visible
                barrier_violated = true;
            t.record(e.value);
            c_received.fetch_add(1, std::memory_order_release);
        });
    });

    std::vector<std::thread> tp;
    for (int p = 0; p < producers; ++p)
        tp.emplace_back([&, p] {
            const std::int64_t base = p * per_producer;
            for (std::int64_t i = 0; i < per_producer; ++i)
                d.publish([v = base + i](Event& e) {
                    e.value = v;
                    e.doubled = 0;
                });
        });
    for (auto& p : tp)
        p.join();

    assert(spin_until_for([&] { return c_received.load(std::memory_order_acquire) == total; }));
    keep_running = false;
    ta.join();
    tb.join();
    tc.join();

    assert(!barrier_violated.load());
    t.check_exactly_once();
    assert(a.last_processed() == total - 1);
    assert(b.last_processed() == total - 1);
    assert(c.last_processed() == total - 1);

    pass("disruptor multi-producer dependency graph");
}

void test_disruptor_multi_producer_move()
{
    // The handle transfers while a consumer thread is running and the ring is
    // live, exactly as in single-producer mode: all shared state is behind the
    // stable core, so the consumer reference and its in-flight batches are
    // untouched. The producers rendezvous around the move because the handle
    // *object* is not a synchronisation point - the same externally
    // synchronized handoff the single-producer contract already asks for.
    constexpr int producers = 3;
    // static, because the producer lambda below has an explicit capture list
    // and no default capture mode - see the note in test_helpers.hpp
    static constexpr std::int64_t per_phase = 3000;     // per producer, per phase
    constexpr std::int64_t total = 2 * producers * per_phase;

    multi_producer_disruptor<Event> d1{64};
    auto& c = d1.add_consumer();

    tally t{total};
    std::atomic<bool> keep_running{true};
    std::atomic<std::int64_t> received{0};

    std::thread tc([&] {
        c.run(keep_running, [&](Event& e, std::int64_t, bool) {
            t.record(e.value);
            received.fetch_add(1, std::memory_order_release);
        });
    });

    auto run_phase = [&](auto& handle, std::int64_t phase_base) {
        std::vector<std::thread> tp;
        for (int p = 0; p < producers; ++p)
            tp.emplace_back([&handle, phase_base, p] {
                const std::int64_t base = phase_base + p * per_phase;
                for (std::int64_t i = 0; i < per_phase; ++i)
                    handle.publish([v = base + i](Event& e) { e.value = v; });
            });
        for (auto& p : tp)
            p.join();
    };

    run_phase(d1, 0);
    multi_producer_disruptor<Event> d2(std::move(d1));      // consumer still running
    run_phase(d2, producers * per_phase);

    assert(spin_until_for([&] { return received.load(std::memory_order_acquire) == total; }));
    keep_running = false;
    tc.join();

    t.check_exactly_once();
    assert(d2.last_published() == total - 1);
    assert(c.last_processed() == total - 1);

    pass("disruptor multi-producer move (handle transfer mid-flight)");
}

template <typename Strategy>
void run_multi_strategy_smoke(const char* name)
{
    constexpr int producers = 3;
    constexpr std::int64_t per_producer = 2000;
    constexpr std::int64_t total = producers * per_producer;

    multi_producer_disruptor<Event, Strategy> d{32};
    auto& c = d.add_consumer();

    tally t{total};
    std::atomic<bool> keep_running{true};
    std::atomic<std::int64_t> received{0};

    std::thread tc([&] {
        c.run(keep_running, [&](Event& e, std::int64_t, bool) {
            t.record(e.value);
            received.fetch_add(1, std::memory_order_release);
        });
    });

    std::vector<std::thread> tp;
    for (int p = 0; p < producers; ++p)
        tp.emplace_back([&, p] {
            const std::int64_t base = p * per_producer;
            for (std::int64_t i = 0; i < per_producer; ++i)
                d.publish([v = base + i](Event& e) { e.value = v; });
        });
    for (auto& p : tp)
        p.join();

    assert(spin_until_for([&] { return received.load(std::memory_order_acquire) == total; }));
    keep_running = false;
    tc.join();
    t.check_exactly_once();

    pass(name);
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

    test_disruptor_multi_producer_out_of_order_publication();
    test_disruptor_multi_producer_exactly_once();
    test_disruptor_multi_producer_gating();
    test_disruptor_multi_producer_wraparound();
    test_disruptor_multi_producer_dependency_graph();
    test_disruptor_multi_producer_move();
    run_multi_strategy_smoke<busy_spin_wait_strategy>(
        "disruptor multi-producer busy-spin wait strategy");
    run_multi_strategy_smoke<yielding_wait_strategy>(
        "disruptor multi-producer yielding wait strategy");
    run_multi_strategy_smoke<blocking_wait_strategy>(
        "disruptor multi-producer blocking wait strategy");
}
