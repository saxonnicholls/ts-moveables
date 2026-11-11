//
//  tests_event_loop.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - unit tests for the event loop (phase 1, POSIX)
//
//  Each test targets a named event-loop failure mode: lost wakeups, wrong-
//  thread mutation, reentrancy corruption, timer drift/bursts, teardown
//  ordering, and unreproducibility (the dispatch tap).
//

#include "test_helpers.hpp"

#include "../TSMoveables/event/loop.hpp"
#include "../TSMoveables/event/clock.hpp"

#if SNICHOLLS_HAS_EVENT_LOOP

#include <atomic>
#include <chrono>
#include <thread>
#include <optional>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace snicholls;
using namespace std::chrono_literals;

namespace {

struct socket_pair {
    int a = -1, b = -1;
    socket_pair() {
        int sv[2];
        assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        a = sv[0];
        b = sv[1];
        // Non-blocking, as a level-triggered reactor's fds should be - a
        // blocking drain loop would hang on the read after the last byte
        ::fcntl(a, F_SETFL, O_NONBLOCK);
        ::fcntl(b, F_SETFL, O_NONBLOCK);
    }
    ~socket_pair() {
        if (a >= 0)
            ::close(a);
        if (b >= 0)
            ::close(b);
    }
};

void drain_fd(int fd)
{
    char buf[256];
    while (::read(fd, buf, sizeof buf) > 0) {
    }
}

void test_loop_fd_events()
{
    event_loop loop;
    socket_pair sp;

    int reads = 0;
    auto w = loop.watch(sp.a, fd_interest::read);
    auto cr = w.on_readable().connect([&] {
        char buf[64];
        [[maybe_unused]] ssize_t n = ::read(sp.a, buf, sizeof buf);
        ++reads;
    });

    // Nothing pending: a bounded run_once dispatches nothing
    assert(!loop.run_once(10ms));
    assert(reads == 0);

    // Data arrives - readable fires exactly once, and (level-triggered)
    // not again once drained
    [[maybe_unused]] ssize_t wr = ::write(sp.b, "hi", 2);
    assert(loop.run_once(1000ms));
    assert(reads == 1);
    assert(!loop.run_once(10ms));
    assert(reads == 1);

    // Writable: a fresh socket is immediately writable; the handler narrows
    // interest back so it does not spin
    int writables = 0;
    w.set_interest(fd_interest::read_write);
    auto cw = w.on_writable().connect([&] {
        ++writables;
        w.set_interest(fd_interest::read);
    });
    assert(loop.run_once(1000ms));
    assert(writables == 1);
    assert(!loop.run_once(10ms));
    assert(writables == 1);

    w.reset();          // unwatch before fds close
    pass("event loop: fd readiness (level-triggered)");
}

void test_loop_eof()
{
    event_loop loop;
    socket_pair sp;

    int eofs = 0;
    auto w = loop.watch(sp.a, fd_interest::read);
    auto c = w.on_readable().connect([&] {
        char buf[64];
        if (::read(sp.a, buf, sizeof buf) == 0)
            ++eofs;
    });

    ::close(sp.b);
    sp.b = -1;
    assert(loop.run_once(1000ms));
    assert(eofs == 1);              // peer close arrives as readable-with-EOF

    w.reset();
    pass("event loop: peer close delivers EOF");
}

void test_loop_timers()
{
    event_loop loop;

    // One-shot fires once, within a generous window, never again
    int fired = 0;
    auto t = loop.after(20ms);
    auto c = t.on_fire().connect([&] { ++fired; });
    const auto t0 = std::chrono::steady_clock::now();
    while (fired == 0 && std::chrono::steady_clock::now() - t0 < 5s)
        loop.run_once(1000ms);
    assert(fired == 1);
    assert(!t.active());            // one-shot is spent... (cancel makes it explicit below)
    loop.run_once(50ms);
    assert(fired == 1);

    // Periodic ticks repeatedly, then cancel() stops it - from this thread
    int ticks = 0;
    auto p = loop.every(5ms);
    auto cp = p.on_fire().connect([&] { ++ticks; });
    while (ticks < 3)
        loop.run_once(1000ms);
    p.cancel();
    const int at = ticks;
    loop.run_once(30ms);
    assert(ticks == at);            // cancelled: no further fires

    // Cancel before first fire
    auto q = loop.after(20ms);
    int never = 0;
    auto cq = q.on_fire().connect([&] { ++never; });
    q.cancel();
    loop.run_once(60ms);
    assert(never == 0);

    pass("event loop: timers (one-shot, periodic, cancel)");
}

void test_loop_post_from_threads()
{
    event_loop loop;
    std::atomic<long long> ran{0};
    std::atomic<bool> wrong_thread{false};

    std::thread lt([&] { loop.run(); });
    spin_until([&] { return loop.running(); });
    const std::thread::id loop_id = lt.get_id();

    constexpr int n_threads = 4;
    constexpr int per_thread = 2000;
    std::vector<std::thread> posters;
    for (int t = 0; t < n_threads; ++t)
        posters.emplace_back([&] {
            for (int i = 0; i < per_thread; ++i)
                loop.post([&] {
                    if (std::this_thread::get_id() != loop_id)
                        wrong_thread.store(true);
                    ran.fetch_add(1, std::memory_order_relaxed);
                });
        });
    for (auto& p : posters)
        p.join();

    spin_until([&] { return ran.load() == n_threads * per_thread; });
    loop.stop();
    lt.join();
    assert(ran.load() == n_threads * per_thread);
    assert(!wrong_thread.load());   // every task ran on the loop thread

    pass("event loop: cross-thread post (no lost wakeups)");
}

// The wake byte is written only when no wake is already pending, so the
// window that matters is the loop going back to sleep just as a post lands.
// Ping-pong puts every post in that window: each round waits until the loop
// has run the previous task (and so has drained and is about to block), then
// posts one more. A lost wakeup shows as a round that never completes; every
// wait has a deadline so it fails instead of hanging. Then several threads
// post bursts while tasks on the loop post follow-ups of their own, which
// exercises the path where the flag is already set by a post from the loop
// thread itself.
void test_loop_coalesced_wakes_lose_nothing()
{
    // Capacity above the most this test ever queues (2 x 4 x 5000): a task on
    // the loop thread that posts into a FULL queue waits for room only the
    // loop thread can make, which is a separate, known limit of post(), not
    // what this test is about
    event_loop loop(1 << 16);
    std::thread lt([&] { loop.run(); });
    assert(spin_until_for([&] { return loop.running(); }, std::chrono::milliseconds(5000)));

    std::atomic<int> done{0};
    constexpr int rounds = 20000;
    for (int r = 1; r <= rounds; ++r) {
        loop.post([&done] { done.fetch_add(1, std::memory_order_release); });
        const bool ok = spin_until_for([&] { return done.load(std::memory_order_acquire) == r; },
                                       std::chrono::milliseconds(5000));
        if (!ok) {
            loop.stop();
            lt.join();
        }
        assert(ok);   // a lost wakeup: the loop slept with this round's task queued
    }

    constexpr int n_threads = 4, per_thread = 5000;
    std::atomic<long long> ran{0};
    std::vector<std::thread> posters;
    for (int t = 0; t < n_threads; ++t)
        posters.emplace_back([&] {
            for (int i = 0; i < per_thread; ++i)
                loop.post([&] {
                    ran.fetch_add(1, std::memory_order_relaxed);
                    loop.post([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });   // from the loop thread
                });
        });
    for (auto& p : posters)
        p.join();
    const bool all = spin_until_for([&] { return ran.load() == 2LL * n_threads * per_thread; },
                                    std::chrono::milliseconds(30000));
    loop.stop();
    lt.join();
    assert(all);

    pass("event loop: coalesced wake writes - ping-pong and nested bursts, nothing lost");
}

void test_loop_thread_contract()
{
    event_loop loop;
    socket_pair sp;

    std::thread lt([&] { loop.run(); });
    spin_until([&] { return loop.running(); });

    // Mutating the loop from a foreign thread while it runs throws - loudly,
    // instead of racing
    assert(throws_logic([&] { (void)loop.watch(sp.a); }));
    assert(throws_logic([&] { (void)loop.after(1ms); }));
    assert(throws_logic([&] { loop.run(); }));          // second run: also loud

    // The sanctioned door works: create the watch via post, on the loop thread
    std::atomic<bool> made{false};
    std::optional<event_loop::fd_watch> w;
    loop.post([&] {
        w.emplace(loop.watch(sp.a, fd_interest::read));
        made.store(true);
    });
    spin_until([&] { return made.load(); });

    std::atomic<int> reads{0};
    std::atomic<bool> wired{false};
    loop.post([&] {
        w->on_readable().connect([&] {
            char buf[16];
            [[maybe_unused]] ssize_t n = ::read(sp.a, buf, sizeof buf);
            reads.fetch_add(1);
        });
        wired.store(true);
    });
    spin_until([&] { return wired.load(); });
    [[maybe_unused]] ssize_t wr = ::write(sp.b, "x", 1);
    spin_until([&] { return reads.load() == 1; });

    // Destroying the watch from this foreign thread marshals through post
    w.reset();
    loop.stop();
    lt.join();

    pass("event loop: thread contract is loud, post is the door in");
}

void test_loop_reentrancy()
{
    // Handlers create and destroy watches and timers mid-dispatch - the
    // classic corruption case, exercised deliberately
    event_loop loop;
    socket_pair sp1;
    socket_pair sp2;

    std::optional<event_loop::fd_watch> w2;
    scoped_connection c2;
    int inner_reads = 0;

    auto w1 = loop.watch(sp1.a, fd_interest::read);
    auto c1 = w1.on_readable().connect([&] {
        drain_fd(sp1.a);
        if (!w2) {                  // create a new watch from inside dispatch
            w2.emplace(loop.watch(sp2.a, fd_interest::read));
            c2 = w2->on_readable().connect([&] {
                drain_fd(sp2.a);
                ++inner_reads;
                w2->reset();        // and destroy it from inside its own handler
            });
        }
    });

    [[maybe_unused]] ssize_t r1 = ::write(sp1.b, "x", 1);
    assert(loop.run_once(1000ms));
    assert(w2.has_value());

    [[maybe_unused]] ssize_t r2 = ::write(sp2.b, "x", 1);
    assert(loop.run_once(1000ms));
    assert(inner_reads == 1);

    // The watch destroyed itself: further writes are not delivered
    [[maybe_unused]] ssize_t r3 = ::write(sp2.b, "x", 1);
    loop.run_once(10ms);
    assert(inner_reads == 1);

    // A timer whose handler cancels a *different* timer mid-dispatch
    int a_fires = 0, b_fires = 0;
    auto ta = loop.after(1ms);
    auto tb = loop.after(1ms);
    auto ca = ta.on_fire().connect([&] {
        ++a_fires;
        tb.cancel();
    });
    auto cb = tb.on_fire().connect([&] { ++b_fires; });
    loop.run_once(1000ms);
    loop.run_once(20ms);
    assert(a_fires == 1);
    assert(b_fires == 0);           // cancelled from inside a's handler

    w1.reset();
    pass("event loop: reentrant create/destroy from handlers");
}

void test_loop_moveability()
{
    // The library's signature, applied to the loop: handles move, the loop
    // handle moves, everything keeps working
    event_loop l1;
    socket_pair sp;

    int reads = 0;
    auto w = l1.watch(sp.a, fd_interest::read);
    auto c = w.on_readable().connect([&] {
        drain_fd(sp.a);
        ++reads;
    });

    std::vector<event_loop::fd_watch> watches;
    watches.push_back(std::move(w));            // watch moves into a container

    event_loop l2(std::move(l1));               // loop handle moves too

    [[maybe_unused]] ssize_t wr = ::write(sp.b, "x", 1);
    assert(l2.run_once(1000ms));
    assert(reads == 1);
    assert(c.connected());

    watches.clear();
    pass("event loop: moveable loop handle and watches");
}

void test_loop_dispatch_tap()
{
    // The replayability hook: every delivery is announced on the tap
    event_loop loop;
    socket_pair sp;

    int tap_tasks = 0, tap_timers = 0, tap_readables = 0;
    auto tc = loop.on_dispatch().connect([&](const dispatch_info& d) {
        switch (d.what) {
        case dispatch_info::kind::task: ++tap_tasks; break;
        case dispatch_info::kind::timer: ++tap_timers; break;
        case dispatch_info::kind::readable: ++tap_readables; break;
        default: break;
        }
    });

    auto w = loop.watch(sp.a, fd_interest::read);
    auto cr = w.on_readable().connect([&] { drain_fd(sp.a); });
    auto t = loop.after(1ms);
    loop.post([] {});
    [[maybe_unused]] ssize_t wr = ::write(sp.b, "x", 1);

    const auto t0 = std::chrono::steady_clock::now();
    while ((tap_tasks < 1 || tap_timers < 1 || tap_readables < 1) &&
           std::chrono::steady_clock::now() - t0 < 5s)
        loop.run_once(1000ms);

    assert(tap_tasks == 1);
    assert(tap_timers == 1);
    assert(tap_readables == 1);

    w.reset();
    pass("event loop: dispatch tap (record everything for replay)");
}

} // namespace

// A stop() that lands immediately after running() must not be lost.
//
// run() used to publish running == true (via run_guard) and only THEN clear
// stop_flag, so a stop in that window was wiped, its wake consumed by the first
// iterate, and the loop parked in iterate(-1) with nothing left to wake it. The
// start-up pattern this test performs is the documented one, which is what made
// it reachable; a downstream consumer's CI hung 8-9 hours on it.
//
// Deliberately bounded. If the stop is lost the loop never exits, and a test
// that simply join()s would reproduce the original failure - a silent multi-hour
// hang - instead of reporting it. So detect the wedge, break it on purpose, and
// fail.
void test_loop_stop_right_after_running_is_not_lost()
{
    for (int i = 0; i < 2000; ++i) {
        event_loop loop;
        std::thread th([&loop] { loop.run(); });
        spin_until([&loop] { return loop.running(); });
        loop.stop();

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        bool stopped = true;
        while (loop.running()) {
            if (std::chrono::steady_clock::now() > deadline) {
                stopped = false;
                break;
            }
            std::this_thread::yield();
        }
        if (!stopped)
            loop.post([&loop] { loop.stop(); });   // unwedge so join() can return
        th.join();
        assert(stopped);
    }

    pass("event_loop: a stop() right after running() is never lost (2000 rounds)");
}

// The flag is cleared when a run ends, so the loop is reusable - and a stop
// issued before a run is honoured rather than discarded, which is the one
// deliberate behaviour change that came with the fix.
void test_loop_stop_flag_lifecycle()
{
    event_loop loop;

    // Run, stop, run again: the second run must not inherit the first's stop.
    for (int round = 0; round < 3; ++round) {
        std::thread th([&loop] { loop.run(); });
        spin_until([&loop] { return loop.running(); });
        std::atomic<bool> ran{false};
        loop.post([&ran] { ran.store(true); });
        spin_until([&ran] { return ran.load(); });   // it really is dispatching
        loop.stop();
        th.join();
        assert(!loop.running());
    }

    // A stop before a run is now honoured: run() returns rather than blocking.
    loop.stop();
    std::thread th([&loop] { loop.run(); });
    th.join();                                      // returns promptly, or this test hangs
    assert(!loop.running());

    pass("event_loop: stop_flag clears on exit, so a loop is reusable");
}

// A manual clock makes a deadline arrive for free.
//
// Timers were nailed to steady_clock, so "does this fire after an hour" cost an
// hour - which in practice means the test gets written as milliseconds and
// checks something subtly different, or does not get written. Here the whole
// hour passes in no wall time at all.
void test_loop_manual_clock_costs_no_wall_time()
{
    event_loop loop;
    auto clk = std::make_shared<manual_clock>();
    loop.set_clock(clk);

    int fired = 0;
    auto t = loop.after(1h);
    auto c = t.on_fire().connect([&] { ++fired; });

    const auto t0 = std::chrono::steady_clock::now();

    loop.run_once();
    assert(fired == 0);                        // nothing due yet

    clk->advance(59min);
    loop.run_once();
    assert(fired == 0);                        // still short

    clk->advance(1min);
    loop.run_once();
    assert(fired == 1);                        // an hour later, exactly

    assert(std::chrono::steady_clock::now() - t0 < 2s);   // and no hour waited

    pass("event_loop: a manual clock fires an hour-long timer in no wall time");
}

// Single-stepping: one advance, one fire, in due order.
void test_loop_manual_clock_single_steps_in_order()
{
    event_loop loop;
    auto clk = std::make_shared<manual_clock>();
    loop.set_clock(clk);

    std::vector<int> order;
    auto a = loop.after(30ms);
    auto ca = a.on_fire().connect([&] { order.push_back(3); });
    auto b = loop.after(10ms);
    auto cb = b.on_fire().connect([&] { order.push_back(1); });
    auto d = loop.after(20ms);
    auto cd = d.on_fire().connect([&] { order.push_back(2); });

    for (int step = 0; step < 3; ++step) {
        clk->advance(10ms);
        loop.run_once();
    }
    assert((order == std::vector<int>{1, 2, 3}));   // due order, not creation order

    // Ties break on the loop's own sequence, so same-deadline timers keep
    // creation order whatever the clock.
    std::vector<int> tied;
    auto t1 = loop.after(5ms);
    auto c1 = t1.on_fire().connect([&] { tied.push_back(1); });
    auto t2 = loop.after(5ms);
    auto c2 = t2.on_fire().connect([&] { tied.push_back(2); });
    clk->advance(5ms);
    loop.run_once();
    assert((tied == std::vector<int>{1, 2}));

    pass("event_loop: a manual clock single-steps, and ties keep creation order");
}

// A repeating timer must not burst when virtual time jumps.
void test_loop_manual_clock_repeats_without_bursting()
{
    event_loop loop;
    auto clk = std::make_shared<manual_clock>();
    loop.set_clock(clk);

    int ticks = 0;
    auto t = loop.every(10ms);
    auto c = t.on_fire().connect([&] { ++ticks; });

    // One big jump fires once and skips ahead, rather than firing ten times -
    // the same stalled-loop rule the real clock follows.
    clk->advance(100ms);
    loop.run_once();
    assert(ticks == 1);

    for (int i = 0; i < 5; ++i) {
        clk->advance(10ms);
        loop.run_once();
    }
    assert(ticks == 6);

    pass("event_loop: a repeating timer does not burst on a virtual jump");
}

// Scaled time: the same timer, finished in a fraction of the wall clock.
void test_loop_scaled_clock_replays_faster()
{
    event_loop loop;
    auto clk = std::make_shared<scaled_clock>(100.0);
    loop.set_clock(clk);

    std::atomic<bool> fired{false};
    auto t = loop.after(2s);
    auto c = t.on_fire().connect([&] { fired.store(true); });

    std::thread th([&loop] { loop.run(); });
    spin_until([&loop] { return loop.running(); });

    // Two virtual seconds at 100x is ~20 real ms. The claim under test is
    // "far less than two seconds", not a precise ratio - a ratio would be a
    // timing test, and therefore flaky.
    const auto t0 = std::chrono::steady_clock::now();
    assert(spin_until_for([&fired] { return fired.load(); }));
    assert(std::chrono::steady_clock::now() - t0 < 1s);

    loop.stop();
    th.join();
    pass("event_loop: a scaled clock replays a 2s timer in well under 2s");
}

// Paused means paused: time stops, so a deadline cannot arrive.
void test_loop_scaled_clock_pauses_and_resumes()
{
    auto clk = std::make_shared<scaled_clock>(1.0);
    const auto before = clk->now();
    clk->pause();
    assert(clk->paused());

    const auto held = clk->now();
    std::this_thread::sleep_for(60ms);
    assert(clk->now() == held);                  // frozen, not merely slowed
    assert(clk->now() >= before);

    clk->resume();
    assert(!clk->paused());
    spin_until([&] { return clk->now() > held; });

    // A speed change re-anchors rather than rewriting time already passed: no
    // jump in either direction.
    auto fast = std::make_shared<scaled_clock>(1.0);
    std::this_thread::sleep_for(50ms);
    const auto at_change = fast->now();
    fast->set_speed(10.0);
    assert(fast->now() >= at_change);
    assert(fast->now() < at_change + 1s);

    pass("scaled_clock: pause freezes time, and a speed change re-anchors");
}

// Nothing changes for a caller who never asks for a clock.
void test_loop_default_clock_is_real_time()
{
    event_loop loop;
    assert(!loop.clock());                       // null means the steady clock

    const auto a = loop.now();
    std::this_thread::sleep_for(20ms);
    assert(loop.now() > a);                      // real time, moving

    std::atomic<bool> fired{false};
    auto t = loop.after(10ms);
    auto c = t.on_fire().connect([&] { fired.store(true); });
    std::thread th([&loop] { loop.run(); });
    spin_until([&loop] { return loop.running(); });
    assert(spin_until_for([&fired] { return fired.load(); }));
    loop.stop();
    th.join();

    pass("event_loop: with no clock set it is real time, as before");
}

void run_event_loop_tests()
{
    test_loop_fd_events();
    test_loop_eof();
    test_loop_timers();
    test_loop_post_from_threads();
    test_loop_coalesced_wakes_lose_nothing();
    test_loop_thread_contract();
    test_loop_reentrancy();
    test_loop_moveability();
    test_loop_dispatch_tap();
    test_loop_stop_right_after_running_is_not_lost();
    test_loop_stop_flag_lifecycle();
    test_loop_default_clock_is_real_time();
    test_loop_manual_clock_costs_no_wall_time();
    test_loop_manual_clock_single_steps_in_order();
    test_loop_manual_clock_repeats_without_bursting();
    test_loop_scaled_clock_replays_faster();
    test_loop_scaled_clock_pauses_and_resumes();
}

#else // !SNICHOLLS_HAS_EVENT_LOOP

// Windows: the reactor is POSIX-only in phase 1 (see FUTURE_DIRECTIONS)
void run_event_loop_tests() {}

#endif
