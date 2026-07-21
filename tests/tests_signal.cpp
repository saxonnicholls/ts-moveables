//
//  tests_signal.cpp
//  TSMoveables
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - unit tests for the thread-safe signal/slot
//

#include "test_helpers.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../TSMoveables/moveable_signal.hpp"

using namespace snicholls;

namespace {

void test_signal_basics()
{
    moveable_signal<int> sig;
    assert(sig.slot_count() == 0);

    std::vector<std::string> order;
    int sum = 0;
    auto c1 = sig.connect([&](int v) { order.push_back("first"); sum += v; });
    auto c2 = sig.connect([&](int v) { order.push_back("second"); sum += v * 10; });
    assert(sig.slot_count() == 2);
    assert(c1.connected() && c2.connected());

    sig(5);
    assert(sum == 55);
    assert((order == std::vector<std::string>{"first", "second"}));    // connection order

    // Disconnect one; the other keeps firing
    c1.disconnect();
    assert(!c1.connected());
    sig(1);
    assert(sum == 65);
    assert(sig.slot_count() == 1);

    // Multiple arguments, reference parameters
    moveable_signal<const std::string&, int> named;
    std::string seen;
    named.connect([&](const std::string& s, int n) { seen = s + std::to_string(n); });
    named("x", 7);
    assert(seen == "x7");

    // disconnect_all
    sig.disconnect_all();
    assert(sig.slot_count() == 0);
    assert(!c2.connected());
    sig(100);
    assert(sum == 65);

    pass("signal basics");
}

void test_signal_scoped_connection()
{
    moveable_signal<> sig;
    int calls = 0;
    {
        scoped_connection sc(sig.connect([&] { ++calls; }));
        assert(sc.connected());
        sig();
        assert(calls == 1);
    }                                       // sc disconnects here
    sig();
    assert(calls == 1);
    assert(sig.slot_count() == 0);

    // release() keeps the connection alive past the scope
    connection kept;
    {
        scoped_connection sc(sig.connect([&] { ++calls; }));
        kept = sc.release();
    }
    sig();
    assert(calls == 2);
    kept.disconnect();

    pass("signal scoped_connection");
}

void test_signal_lifetime_tracking()
{
    struct Receiver {
        int received = 0;
        void on_event(int v) { received += v; }
    };

    moveable_signal<int> sig;
    auto obj = std::make_shared<Receiver>();
    sig.connect(obj, &Receiver::on_event);

    sig(3);
    assert(obj->received == 3);
    assert(sig.slot_count() == 1);

    // Object dies - the slot silently disconnects
    Receiver* raw = obj.get();
    (void)raw;
    obj.reset();
    sig(5);                                 // must not touch the dead object
    assert(sig.slot_count() == 0);

    // weak_ptr overload with a plain lambda
    auto guard = std::make_shared<int>(0);
    int fired = 0;
    sig.connect(std::weak_ptr<const void>(guard), [&](int) { ++fired; });
    sig(1);
    assert(fired == 1);
    guard.reset();
    sig(1);
    assert(fired == 1);

    pass("signal lifetime tracking");
}

void test_signal_reentrancy()
{
    // A slot that connects another slot: the new slot is not called during
    // this emission (snapshot semantics), but is called on the next
    moveable_signal<> sig;
    int second_calls = 0;
    connection inner;
    bool connected_inner = false;
    sig.connect([&] {
        if (!connected_inner) {
            connected_inner = true;
            inner = sig.connect([&] { ++second_calls; });
        }
    });
    sig();
    assert(second_calls == 0);
    sig();
    assert(second_calls == 1);

    // A slot that disconnects a later slot: the later slot is skipped for the
    // remainder of the emission
    moveable_signal<> sig2;
    int late_calls = 0;
    connection late;
    sig2.connect([&] { late.disconnect(); });
    late = sig2.connect([&] { ++late_calls; });
    sig2();
    assert(late_calls == 0);

    // A slot that re-emits (bounded recursion): no deadlock
    moveable_signal<int> sig3;
    int depth_hits = 0;
    sig3.connect([&](int depth) {
        ++depth_hits;
        if (depth > 0)
            sig3(depth - 1);
    });
    sig3(3);
    assert(depth_hits == 4);

    pass("signal reentrancy (connect/disconnect/emit from slots)");
}

void test_signal_threaded()
{
    // Emissions from many threads, all counted exactly
    constexpr int n_threads = 4;
    constexpr int n_emits = 10000;
    moveable_signal<> sig;
    std::atomic<long long> count{0};
    sig.connect([&] { ++count; });

    std::vector<std::thread> threads;
    for (int t = 0; t < n_threads; ++t)
        threads.emplace_back([&] {
            for (int i = 0; i < n_emits; ++i)
                sig();
        });
    for (auto& t : threads)
        t.join();
    assert(count.load() == static_cast<long long>(n_threads) * n_emits);

    // Emitters racing connect/disconnect churn: exercised for safety, not counts
    std::atomic<bool> keep_running{true};
    std::atomic<long long> churn_seen{0};
    std::thread churner([&] {
        while (keep_running.load()) {
            auto c = sig.connect([&] { ++churn_seen; });
            c.disconnect();
        }
    });
    for (int i = 0; i < 50000; ++i)
        sig();
    keep_running = false;
    churner.join();
    assert(count.load() == static_cast<long long>(n_threads) * n_emits + 50000);

    pass("signal threaded emission");
}

void test_signal_moveability()
{
    // The library's reason to exist: a signal member moves with its owner and
    // connections stay live
    struct Button {
        moveable_signal<int> clicked;
    };

    Button b1;
    int received = 0;
    auto c = b1.clicked.connect([&](int v) { received += v; });

    std::vector<Button> panel;
    panel.push_back(std::move(b1));         // move into a container
    panel.reserve(32);                      // force a reallocation move too
    panel[0].clicked(7);
    assert(received == 7);
    assert(c.connected());                  // handle survived both moves

    // Moved-from signal is a safe no-op
    moveable_signal<int> a;
    a.connect([&](int v) { received += v; });
    moveable_signal<int> b(std::move(a));
    a(100);                                 // no-op, no crash
    assert(received == 7);
    assert(a.slot_count() == 0);
    b(1);
    assert(received == 8);

    pass("signal moveability");
}

} // namespace

void run_signal_tests()
{
    test_signal_basics();
    test_signal_scoped_connection();
    test_signal_lifetime_tracking();
    test_signal_reentrancy();
    test_signal_threaded();
    test_signal_moveability();
}
