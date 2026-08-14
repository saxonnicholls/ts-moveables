//
//  tests_websocket_client.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - unit tests for the outbound WebSocket client
//
//  The claims worth testing are the ones that are easy to get wrong and quiet
//  when they are: that client frames go out masked (a real server closes on an
//  unmasked one), that the handshake is verified rather than assumed, that
//  fragments reassemble, and - the whole reason this component exists - that it
//  comes back on its own after the upstream disappears.
//
//  The server under test is ours: the library talking to itself over a real
//  socket, which is the only way the masking rule gets exercised in both
//  directions at once.
//

#include "test_helpers.hpp"

#include "../TSMoveables/http/websocket_client.hpp"
#include "../TSMoveables/http/ws_broadcast_hub.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace snicholls;
using namespace snicholls::http;
using namespace std::chrono_literals;

namespace {

#if !SNICHOLLS_HAS_WEBSOCKET_CLIENT

void run_all() { pass("websocket_client: POSIX only - skipped"); }

#else

// An echo server on its own loop, startable and stoppable so a test can pull
// the upstream out from under a client and watch it come back.
struct echo_server {
    server srv;
    std::uint16_t port = 0;
    std::thread th;
    std::atomic<int> connections{0};

    explicit echo_server(std::uint16_t fixed = 0)
    {
        srv.get("/feed", websocket_route([this](websocket ws) {
            connections.fetch_add(1, std::memory_order_relaxed);
            ws.on_message([](websocket sock, const ws_message& m) { sock.send_text(m.data); });
        }));
        port = srv.listen("127.0.0.1", fixed);
        assert(port != 0);
    }

    void start()
    {
        th = std::thread([this] { srv.run(); });
        spin_until([this] { return srv.running(); });
    }

    void stop()
    {
        srv.stop();
        if (th.joinable())
            th.join();
    }

    ~echo_server() { stop(); }
};

// A loop on its own thread, so the client under test runs where it would in a
// real program rather than being pumped by the test body.
struct loop_thread {
    event_loop loop;
    std::thread th;

    void start()
    {
        th = std::thread([this] { loop.run(); });
        spin_until([this] { return loop.running(); });
    }
    void stop()
    {
        loop.stop();
        if (th.joinable())
            th.join();
    }
    ~loop_thread() { stop(); }
};

void test_ws_client_url_parsing()
{
    http::detail::ws_url u;
    assert(http::detail::parse_ws_url("ws://host:9000/stream", u));
    assert(u.host == "host" && u.port == "9000" && u.path == "/stream" && !u.secure);

    assert(http::detail::parse_ws_url("ws://host/x", u));
    assert(u.port == "80");                         // the scheme's default

    assert(http::detail::parse_ws_url("wss://host/x", u));
    assert(u.port == "443" && u.secure);

    assert(http::detail::parse_ws_url("ws://host", u));
    assert(u.path == "/");                          // an absent path is root

    // An IPv6 literal's colons belong to the address, not the port
    assert(http::detail::parse_ws_url("ws://[::1]:8080/f", u));
    assert(u.host == "::1" && u.port == "8080");

    // Refused rather than guessed - a guess here connects somewhere the caller
    // did not ask for
    assert(!http::detail::parse_ws_url("http://host/x", u));
    assert(!http::detail::parse_ws_url("host:80/x", u));
    assert(!http::detail::parse_ws_url("ws://", u));

    pass("websocket_client: URL parsing, defaults, IPv6, and refusals");
}

void test_ws_client_masks_its_frames()
{
    // RFC 6455 §5.3. The bit is not cosmetic: an unmasked client frame is a
    // protocol error every conforming server closes on, so assert the encoder
    // sets it AND that the payload is genuinely transformed.
    const std::string body = "hello";
    const std::string f = http::detail::ws_frame_masked(ws_opcode::text, body.data(), body.size(),
                                                  0xDEADBEEFu);
    assert((static_cast<unsigned char>(f[1]) & 0x80) != 0);       // MASK bit
    assert((static_cast<unsigned char>(f[1]) & 0x7f) == body.size());
    assert(f.size() == 2 + 4 + body.size());

    const unsigned char k[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    for (std::size_t i = 0; i < 4; ++i)
        assert(static_cast<unsigned char>(f[2 + i]) == k[i]);
    std::string got;
    for (std::size_t i = 0; i < body.size(); ++i)
        got.push_back(char(static_cast<unsigned char>(f[6 + i]) ^ k[i & 3]));
    assert(got == body);                                          // unmasks to the original
    assert(f.compare(6, body.size(), body) != 0);                 // and was not sent in clear

    pass("websocket_client: client frames are masked, and unmask to the original");
}

void test_ws_client_round_trip()
{
    echo_server s;
    s.start();
    loop_thread lt;

    std::atomic<int> got{0};
    std::string last;
    moveable_mutex<> m;
    std::atomic<bool> opened{false};

    websocket_client c;
    c.on_open([&opened] { opened.store(true); });
    c.on_message([&](const ws_message& msg) {
        std::lock_guard<moveable_mutex<>> g(m);
        last = msg.data;
        got.fetch_add(1);
    });
    assert(c.connect(lt.loop, "ws://127.0.0.1:" + std::to_string(s.port) + "/feed"));
    lt.start();

    assert(spin_until_for([&] { return opened.load(); }));
    c.send_text("ping-1");
    assert(spin_until_for([&] { return got.load() == 1; }));
    {
        std::lock_guard<moveable_mutex<>> g(m);
        assert(last == "ping-1");
    }

    // A payload over 125 bytes takes the 16-bit length form - the boundary the
    // encoder is most likely to get wrong
    const std::string big(1000, 'z');
    c.send_text(big);
    assert(spin_until_for([&] { return got.load() == 2; }));
    {
        std::lock_guard<moveable_mutex<>> g(m);
        assert(last == big);
    }

    const auto st = c.snapshot();
    assert(st.connects == 1 && st.messages_in == 2 && st.messages_out == 2);

    c.close();
    lt.stop();
    pass("websocket_client: connects, verifies the handshake, echoes both length forms");
}

void test_ws_client_rejects_a_non_websocket_endpoint()
{
    // Anything can answer a GET. Only the endpoint that saw our nonce can
    // produce the right Sec-WebSocket-Accept, so a plain 200 must not be
    // mistaken for an upgrade.
    server srv;
    srv.get("/feed", [](const request&, responder r) { r.send(200, "text/plain", "not a ws"); });
    const std::uint16_t port = srv.listen("127.0.0.1", 0);
    assert(port != 0);
    std::thread th([&srv] { srv.run(); });
    spin_until([&srv] { return srv.running(); });

    loop_thread lt;
    std::atomic<int> closes{0};
    std::atomic<bool> wrong_status{false};

    ws_client_config cfg;
    cfg.auto_reconnect = false;             // one attempt, so the result is the verdict
    websocket_client c{cfg};
    c.on_close([&](ws_client_status why) {
        if (why != ws_client_status::handshake_failed)
            wrong_status.store(true);
        closes.fetch_add(1);
    });
    c.on_open([&wrong_status] { wrong_status.store(true); });   // must never fire

    assert(c.connect(lt.loop, "ws://127.0.0.1:" + std::to_string(port) + "/feed"));
    lt.start();
    assert(spin_until_for([&] { return closes.load() == 1; }));
    assert(!wrong_status.load());
    assert(!c.connected());

    lt.stop();
    srv.stop();
    th.join();
    pass("websocket_client: a 200 is not an upgrade - the accept key is verified");
}

void test_ws_client_reconnects_after_the_upstream_dies()
{
    // The reason this component exists. Bring the upstream up, connect, kill
    // it, bring it back on the SAME port, and require the client to return
    // without anyone asking it to.
    auto s = std::unique_ptr<echo_server>(new echo_server());
    const std::uint16_t port = s->port;
    s->start();

    loop_thread lt;
    std::atomic<int> opens{0}, closes{0};

    ws_client_config cfg;
    cfg.min_backoff = 20ms;                 // keep the test quick; the policy is unchanged
    cfg.max_backoff = 200ms;
    cfg.session_grace = 1ms;
    websocket_client c{cfg};
    c.on_open([&opens] { opens.fetch_add(1); });
    c.on_close([&closes](ws_client_status) { closes.fetch_add(1); });

    assert(c.connect(lt.loop, "ws://127.0.0.1:" + std::to_string(port) + "/feed"));
    lt.start();
    assert(spin_until_for([&] { return opens.load() == 1; }));

    // The upstream goes away
    s->stop();
    s.reset();
    assert(spin_until_for([&] { return closes.load() >= 1; }));

    // ... and comes back on the same port. Nothing tells the client.
    auto s2 = std::unique_ptr<echo_server>(new echo_server(port));
    s2->start();

    assert(spin_until_for([&] { return opens.load() >= 2; }, 30s));
    assert(c.connected());

    // And it still works, which is the part that matters
    std::atomic<int> got{0};
    c.on_message([&got](const ws_message&) { got.fetch_add(1); });
    c.send_text("after-the-outage");
    assert(spin_until_for([&] { return got.load() >= 1; }));

    assert(c.snapshot().reconnects >= 1);

    c.close();
    lt.stop();
    pass("websocket_client: survives an upstream restart and reconnects itself");
}

void test_ws_client_close_is_final()
{
    // A deliberate close must not be treated as a failure, or shutdown races
    // the backoff timer and the process never quiets down.
    echo_server s;
    s.start();
    loop_thread lt;

    std::atomic<int> opens{0};
    websocket_client c;
    c.on_open([&opens] { opens.fetch_add(1); });
    assert(c.connect(lt.loop, "ws://127.0.0.1:" + std::to_string(s.port) + "/feed"));
    lt.start();
    assert(spin_until_for([&] { return opens.load() == 1; }));

    c.close();
    spin_until_for([&] { return !c.connected(); });
    std::this_thread::sleep_for(300ms);     // several backoff windows at the default
    assert(opens.load() == 1);              // it stayed shut
    assert(!c.connected());

    lt.stop();
    pass("websocket_client: close() is final - no reconnect follows");
}

void test_ws_client_relays_into_the_hub()
{
    // The N-in half joined to the M-out half: an upstream feed arrives on a
    // client and leaves through the hub's fan-out, which is the whole point of
    // building the client at all.
    echo_server up;
    up.start();
    loop_thread lt;

    ws_broadcast_hub hub;
    std::atomic<int> relayed{0};

    websocket_client c;
    c.on_message([&](const ws_message& m) {
        hub.publish("relayed", m.data);
        relayed.fetch_add(1);
    });
    assert(c.connect(lt.loop, "ws://127.0.0.1:" + std::to_string(up.port) + "/feed"));
    lt.start();

    spin_until_for([&] { return c.connected(); });
    c.send_text("upstream-event");           // echoed back, so it arrives as inbound
    assert(spin_until_for([&] { return relayed.load() >= 1; }));

    assert(hub.snapshot().published >= 1);

    c.close();
    lt.stop();
    pass("websocket_client: relays an upstream feed into the broadcast hub");
}

void run_all()
{
    test_ws_client_url_parsing();
    test_ws_client_masks_its_frames();
    test_ws_client_round_trip();
    test_ws_client_rejects_a_non_websocket_endpoint();
    test_ws_client_reconnects_after_the_upstream_dies();
    test_ws_client_close_is_final();
    test_ws_client_relays_into_the_hub();
}

#endif

} // namespace

void run_websocket_client_tests()
{
    run_all();
}
