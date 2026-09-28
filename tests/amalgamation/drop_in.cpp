//
//  drop_in.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  The single-header drop-in, used the way the README says to use it: this
//  file sees ONLY single_include/ (the Makefile compiles it with nothing else
//  on the include path), and it names every shipped component.
//
//  The old check was a one-line HTTP/1.1 server, which proved the file was
//  fresh and compiled and nothing about what was in it - so HTTP/2 went a
//  whole release missing from the drop-in while h2spec graded it 147/147. A
//  smoke test that only touches the part that already worked cannot report
//  on the part that did not. Here each component is constructed and does one
//  real thing, so leaving one out of the umbrella is a compile error.
//
//  scripts/check_amalgamation.py asks the same question from the other side
//  (every header in the tree is in the file); this proves what is in the file
//  actually works on its own.
//

#include "ts_moveables.hpp"

#include <cassert>
#include <cstdio>
#include <string>

// Each platform-gated header defines its macro as 0 or 1. Undefined means the
// header never made it into the amalgamation - the http2.hpp failure exactly.
#if !defined(SNICHOLLS_HAS_EVENT_LOOP) || !defined(SNICHOLLS_HAS_TIME_MASTER) ||      \
    !defined(SNICHOLLS_HAS_HTTP_SERVER) || !defined(SNICHOLLS_HAS_HTTP2) ||           \
    !defined(SNICHOLLS_HAS_WEBSOCKET) || !defined(SNICHOLLS_HAS_WS_BROADCAST_HUB) ||  \
    !defined(SNICHOLLS_HAS_WEBSOCKET_CLIENT)
#error "a platform-gated component is missing from single_include/ts_moveables.hpp"
#endif

// Opt-in headers link a third party; the umbrella must not drag them in
#if defined(SNICHOLLS_HAS_WS_DEFLATE) || defined(SNICHOLLS_HAS_TLS)
#error "an opt-in header (zlib / TLS) leaked into single_include/ts_moveables.hpp"
#endif

using namespace snicholls;

int main()
{
    // the moveable primitives - each moved, which is the whole point
    moveable_atomic<int> a{1};
    moveable_atomic<int> a2 = std::move(a);
    moveable_mutex<> m;
    moveable_mutex<> m2 = std::move(m);
    { std::lock_guard<moveable_mutex<>> g(m2); }
    moveable_spin_lock sl;
    moveable_spin_lock sl2 = std::move(sl);
    moveable_condition_variable cv;
    moveable_condition_variable cv2 = std::move(cv);
    moveable_once_flag once;
    moveable_semaphore sem{1};
    moveable_latch latch{1};
    latch.count_down();
    moveable_barrier<> barrier{1};
    barrier.arrive_and_wait();
    (void)a2; (void)sl2; (void)cv2; (void)once; (void)sem;

    // concurrent/
    synchronized<int> s{41};
    s.with_lock([](int& v) { ++v; });
    assert(s.load() == 42);

    circular_buffer<int> ring{4};
    assert(ring.try_push(7));

    mpmc_queue<int> q{4};
    assert(q.push(7));

    disruptor<int> d{8};
    (void)d;

    intern_pool<std::string> pool;
    auto i1 = pool.intern(std::string("same"));
    auto i2 = pool.intern(std::string("same"));
    assert(i1.get() == i2.get());

    work_stealing_task_pool tp{2};
    moveable_atomic<int> sum{0};
    parallel_for(tp, 0, 64, [&](int) { sum.fetch_add(1); });
    assert(sum.load() == 64);

    int unrolled = 0;
    constexpr_for<4>([&](auto I) { unrolled += I.value; });
    assert(unrolled == 6);

    moveable_signal<int> sig;
    int got = 0;
    auto conn = sig.connect([&](int v) { got = v; });
    sig(3);
    assert(got == 3);

    log::logger lg;
    (void)lg;

#if SNICHOLLS_HAS_TIME_MASTER
    time_master tm;
    (void)tm;
#endif

#if SNICHOLLS_HAS_HTTP_SERVER
    http::server srv;
    srv.get("/", [](const auto&, auto r) { r.send(200, "text/plain", "ok"); });
  #if SNICHOLLS_HAS_HTTP2
    http::enable_http2(srv, http::h2_config{});
  #endif
  #if SNICHOLLS_HAS_WEBSOCKET
    srv.get("/ws", http::websocket_route([](http::websocket) {}));
  #endif
  #if SNICHOLLS_HAS_WS_BROADCAST_HUB
    http::ws_broadcast_hub hub;
    hub.mount(srv);
    hub.publish("t", "{}");
  #endif
  #if SNICHOLLS_HAS_WEBSOCKET_CLIENT
    http::websocket_client client;
    (void)client;
  #endif
    if (!srv.listen("127.0.0.1", 0))
        return 1;
#endif

    std::printf("single-header drop-in %s: every shipped component builds and runs\n",
                version_string());
    return 0;
}
