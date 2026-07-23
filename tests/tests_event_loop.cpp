//
//  tests_event_loop.cpp
//  TSMoveables
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - unit tests for the event loop (phase 1, POSIX)
//
//  Each test targets a named event-loop failure mode: lost wakeups, wrong-
//  thread mutation, reentrancy corruption, timer drift/bursts, teardown
//  ordering, and unreproducibility (the dispatch tap).
//

#include "test_helpers.hpp"

#include "../TSMoveables/event_loop.hpp"

#if SNICHOLLS_HAS_EVENT_LOOP

#include <atomic>
#include <chrono>
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

void run_event_loop_tests()
{
    test_loop_fd_events();
    test_loop_eof();
    test_loop_timers();
    test_loop_post_from_threads();
    test_loop_thread_contract();
    test_loop_reentrancy();
    test_loop_moveability();
    test_loop_dispatch_tap();
}

#else // !SNICHOLLS_HAS_EVENT_LOOP

// Windows: the reactor is POSIX-only in phase 1 (see FUTURE_DIRECTIONS)
void run_event_loop_tests() {}

#endif
