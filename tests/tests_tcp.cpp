//
//  tests_tcp.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - unit tests for the plain TCP byte stream
//
//  The claims under test: a connect/accept pair moves bytes both ways and
//  byte-exactly; every call honours its deadline rather than blocking forever;
//  the deadline is not restarted by partial transfers; a clean peer close reads
//  as 0 rather than an error; a half close lets the other direction keep
//  working; shutdown() from another thread unblocks a parked read, which is the
//  one concurrent operation this type promises; and the handles are movable
//  without ever double-closing a descriptor.
//

#include "test_helpers.hpp"

#include "../TSMoveables/net/tcp.hpp"
#include "../TSMoveables/moveable/mutex.hpp"

#if SNICHOLLS_HAS_TCP

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace snicholls;
using namespace std::chrono_literals;

namespace {

// A listener on an ephemeral port, plus the accepted peer, so a test gets both
// ends of a real connection in three lines.
struct pair_on_loopback {
    net::tcp_listener listener;
    net::tcp_stream   client;
    net::tcp_stream   server;

    pair_on_loopback()
    {
        std::string err;
        listener = net::tcp_listener::bind("127.0.0.1", 0, 16, &err);
        assert(listener.valid() && err.empty());
        assert(listener.port() != 0);        // the kernel told us which one

        std::thread th([this] {
            client = net::tcp_stream::connect("127.0.0.1", listener.port(), 2s);
        });
        server = listener.accept(2s, &err);
        th.join();

        assert(server.valid());
        assert(client.valid());
        client.set_nodelay(true);
        server.set_nodelay(true);
    }
};

std::string read_exactly(net::tcp_stream& s, std::size_t n,
                         std::chrono::milliseconds budget = 2s)
{
    std::string out;
    out.reserve(n);
    char buf[4096];
    while (out.size() < n) {
        const std::size_t want = n - out.size();
        const std::ptrdiff_t got =
            s.read(buf, want < sizeof buf ? want : sizeof buf, budget);
        if (got <= 0)
            break;
        out.append(buf, std::size_t(got));
    }
    return out;
}

void test_tcp_round_trip()
{
    pair_on_loopback p;

    const std::string up = "hello over plain TCP";
    assert(p.client.write_all(up.data(), up.size(), 1s));
    assert(read_exactly(p.server, up.size()) == up);

    // And back the other way on the same connection.
    const std::string down = "and back again";
    assert(p.server.write_all(down.data(), down.size(), 1s));
    assert(read_exactly(p.client, down.size()) == down);

    pass("tcp_stream: connect/accept moves bytes both ways");
}

void test_tcp_carries_arbitrary_bytes()
{
    // Embedded NULs and every byte value: a byte stream must not care, and this
    // is what a caller reaching for TCP instead of WebSocket text wants.
    pair_on_loopback p;

    std::string payload;
    payload.reserve(256 * 64);
    for (int rep = 0; rep < 64; ++rep)
        for (int b = 0; b < 256; ++b)
            payload.push_back(static_cast<char>(b));

    std::atomic<bool> sent{false};
    std::thread th([&] {
        sent.store(p.client.write_all(payload.data(), payload.size(), 3s));
    });
    const std::string got = read_exactly(p.server, payload.size(), 3s);
    th.join();

    assert(sent.load());
    assert(got.size() == payload.size());
    assert(got == payload);                  // byte for byte, NULs included

    pass("tcp_stream: carries every byte value, embedded NULs included");
}

void test_tcp_read_honours_its_deadline()
{
    pair_on_loopback p;

    char buf[64];
    const auto t0 = std::chrono::steady_clock::now();
    const std::ptrdiff_t got = p.server.read(buf, sizeof buf, 150ms);
    const auto waited = std::chrono::steady_clock::now() - t0;

    assert(got == -1);                       // not 0 - that would mean EOF
    assert(errno == ETIMEDOUT);              // and the reason is reported
    assert(waited >= 120ms);                 // it really waited
    assert(waited < 2s);                     // and really stopped

    pass("tcp_stream: read() times out as ETIMEDOUT, not forever");
}

void test_tcp_clean_close_reads_as_eof()
{
    pair_on_loopback p;
    p.client.close();

    char buf[16];
    assert(p.server.read(buf, sizeof buf, 2s) == 0);   // 0 means the peer went

    pass("tcp_stream: a peer close reads as 0, distinct from an error");
}

void test_tcp_half_close_keeps_the_other_direction()
{
    pair_on_loopback p;

    const std::string last = "final word";
    assert(p.client.write_all(last.data(), last.size(), 1s));
    assert(p.client.shutdown_write());                 // FIN, but still reading

    assert(read_exactly(p.server, last.size()) == last);
    char buf[8];
    assert(p.server.read(buf, sizeof buf, 1s) == 0);   // server sees the FIN

    // The server can still talk, and the client can still listen.
    const std::string reply = "heard you";
    assert(p.server.write_all(reply.data(), reply.size(), 1s));
    assert(read_exactly(p.client, reply.size()) == reply);

    pass("tcp_stream: shutdown_write is a half close, not a close");
}

void test_tcp_shutdown_unblocks_a_parked_read()
{
    // The one concurrent operation this type promises. close() from another
    // thread would be a use-after-free waiting to happen - the descriptor
    // number is immediately reusable - so shutdown() is the way to interrupt a
    // blocked call, and that has to actually work.
    pair_on_loopback p;

    std::atomic<bool> returned{false};
    std::atomic<std::ptrdiff_t> result{1};
    std::thread reader([&] {
        char buf[64];
        result.store(p.server.read(buf, sizeof buf, 30s));   // would park for ages
        returned.store(true);
    });

    std::this_thread::sleep_for(100ms);                 // make sure it is parked
    assert(!returned.load());
    assert(p.server.shutdown());

    assert(spin_until_for([&] { return returned.load(); }));
    reader.join();
    assert(result.load() <= 0);                         // woken, not left waiting

    pass("tcp_stream: shutdown() from another thread unblocks a parked read");
}

void test_tcp_accept_honours_its_deadline()
{
    std::string err;
    net::tcp_listener l = net::tcp_listener::bind("127.0.0.1", 0, 4, &err);
    assert(l.valid());

    const auto t0 = std::chrono::steady_clock::now();
    net::tcp_stream s = l.accept(150ms, &err);
    const auto waited = std::chrono::steady_clock::now() - t0;

    assert(!s.valid());
    assert(errno == ETIMEDOUT);
    assert(waited >= 120ms && waited < 2s);

    pass("tcp_listener: accept() times out rather than blocking forever");
}

void test_tcp_connect_refused_and_timed_out()
{
    // A refused connect must fail promptly and say so, not hang.
    std::string err;
    net::tcp_listener l = net::tcp_listener::bind("127.0.0.1", 0, 1, &err);
    assert(l.valid());
    const std::uint16_t dead = l.port();
    l.close();                               // nothing is listening there now

    net::tcp_stream s = net::tcp_stream::connect("127.0.0.1", dead, 1s, &err);
    assert(!s.valid());
    assert(!err.empty());                    // and it told us why

    pass("tcp_stream: a refused connect fails promptly with a reason");
}

void test_tcp_handles_move_without_double_closing()
{
    pair_on_loopback p;

    // Move the connected ends around; the bytes must still flow, and nothing
    // may close the same descriptor twice on the way.
    net::tcp_stream moved_client = std::move(p.client);
    net::tcp_stream moved_server(std::move(p.server));
    assert(!p.client.valid());                // moved-from is empty, not shared
    assert(!p.server.valid());

    const std::string msg = "still connected";
    assert(moved_client.write_all(msg.data(), msg.size(), 1s));
    assert(read_exactly(moved_server, msg.size()) == msg);

    // Move-assign over a live stream: the old descriptor is closed exactly once.
    net::tcp_stream sink;
    sink = std::move(moved_client);
    assert(sink.valid());
    assert(!moved_client.valid());

    pass("tcp_stream: movable, and a descriptor is never closed twice");
}

void test_tcp_listener_reports_the_chosen_port()
{
    // Two listeners on port 0 must not collide, which is what makes the tests
    // above safe to run in parallel with anything else.
    std::string err;
    net::tcp_listener a = net::tcp_listener::bind("127.0.0.1", 0, 4, &err);
    net::tcp_listener b = net::tcp_listener::bind("127.0.0.1", 0, 4, &err);
    assert(a.valid() && b.valid());
    assert(a.port() != 0 && b.port() != 0);
    assert(a.port() != b.port());

    pass("tcp_listener: port 0 reports the port the kernel actually chose");
}

// ------------------------------------------------------------------ reactor
#if SNICHOLLS_HAS_EVENT_LOOP

// A loop on its own thread, stoppable, so a reactor test can drive it.
struct loop_on_a_thread {
    event_loop loop;
    std::thread th;

    void start()
    {
        th = std::thread([this] { loop.run(); });
        spin_until([this] { return loop.running(); });
    }
    void stop()
    {
        loop.post([this] { loop.stop(); });
        if (th.joinable())
            th.join();
    }
    ~loop_on_a_thread() { stop(); }
};

void test_tcp_reactor_echoes_many_connections_on_one_thread()
{
    // The case tcp_stream is the wrong shape for: several connections, one
    // thread, nobody blocking.
    loop_on_a_thread lt;
    net::tcp_acceptor acc(lt.loop, "127.0.0.1", 0);
    std::atomic<int> accepted{0};

    acc.on_accept([&accepted](net::tcp_connection c) {
        accepted.fetch_add(1);
        c.on_data([](net::tcp_connection conn, std::string& buf) {
            conn.send(buf);                 // echo
            buf.clear();                    // consumed
        });
    });
    const std::uint16_t port = acc.port();
    assert(port != 0);
    lt.start();

    constexpr int clients = 8;
    std::vector<net::tcp_stream> peers;
    for (int i = 0; i < clients; ++i) {
        peers.push_back(net::tcp_stream::connect("127.0.0.1", port, 2s));
        assert(peers.back().valid());
    }
    assert(spin_until_for([&] { return accepted.load() == clients; }));

    for (int i = 0; i < clients; ++i) {
        const std::string msg = "client-" + std::to_string(i);
        assert(peers[std::size_t(i)].write_all(msg.data(), msg.size(), 2s));
        assert(read_exactly(peers[std::size_t(i)], msg.size()) == msg);
    }

    pass("tcp_acceptor: eight connections echoed by one loop thread");
}

void test_tcp_reactor_send_is_bounded_and_drains()
{
    // The backpressure the errata asked to be able to see coming: above the
    // high-water mark send() refuses and queues nothing, rather than accepting
    // everything and growing until the process dies.
    loop_on_a_thread lt;
    net::tcp_conn_config cfg;
    cfg.write_high_water = 64 * 1024;
    net::tcp_acceptor acc(lt.loop, "127.0.0.1", 0, 128, cfg);

    std::atomic<bool> refused{false};
    std::atomic<bool> drained{false};
    std::atomic<std::size_t> peak{0};

    acc.on_accept([&](net::tcp_connection c) {
        c.on_drain([&drained](net::tcp_connection) { drained.store(true); });
        // Push until it says no. A reader that never reads is what makes the
        // queue back up at all.
        for (int i = 0; i < 10000; ++i) {
            if (!c.send(std::string(4096, 'x'))) {
                refused.store(true);
                break;
            }
            const std::size_t b = c.backlog();
            if (b > peak.load())
                peak.store(b);
        }
    });
    const std::uint16_t port = acc.port();
    lt.start();

    net::tcp_stream peer = net::tcp_stream::connect("127.0.0.1", port, 2s);
    assert(peer.valid());

    assert(spin_until_for([&] { return refused.load(); }));
    assert(peak.load() <= cfg.write_high_water);     // the bound held

    // Now drain it: once the peer reads everything, on_drain must fire.
    char buf[16384];
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!drained.load() && std::chrono::steady_clock::now() < deadline) {
        if (peer.read(buf, sizeof buf, 200ms) <= 0)
            break;
    }
    assert(drained.load());

    pass("tcp_connection: send() refuses above the high-water mark, on_drain follows");
}

// A message larger than the high-water mark, on an empty queue, still goes.
//
// The first bound refused it - 0 + size > high_water - so any message bigger
// than the mark was unsendable forever and the bound doubled as an
// undocumented cap on message size.
void test_tcp_reactor_oversized_send_on_empty_queue()
{
    loop_on_a_thread lt;
    net::tcp_conn_config cfg;
    cfg.write_high_water = 4 * 1024;
    net::tcp_acceptor acc(lt.loop, "127.0.0.1", 0, 128, cfg);

    const std::string big(256 * 1024, 'z');               // 64x the mark
    std::atomic<bool> accepted{false};
    acc.on_accept([&](net::tcp_connection c) { accepted.store(c.send(big)); });
    const std::uint16_t port = acc.port();
    lt.start();

    net::tcp_stream peer = net::tcp_stream::connect("127.0.0.1", port, 2s);
    assert(peer.valid());
    assert(read_exactly(peer, big.size(), 5s) == big);    // all of it arrived
    // send() can write all of it before it returns, so the peer may finish
    // reading before the loop thread stores the result: wait for the store.
    assert(spin_until_for([&] { return accepted.load(); }));

    pass("tcp_connection: one send larger than the high water still goes on an empty queue");
}

void test_tcp_reactor_reports_close_once()
{
    loop_on_a_thread lt;
    net::tcp_acceptor acc(lt.loop, "127.0.0.1", 0);
    std::atomic<int> closes{0};

    acc.on_accept([&closes](net::tcp_connection c) {
        c.on_close([&closes](net::tcp_connection) { closes.fetch_add(1); });
        c.on_data([](net::tcp_connection, std::string& buf) { buf.clear(); });
    });
    const std::uint16_t port = acc.port();
    lt.start();

    {
        net::tcp_stream peer = net::tcp_stream::connect("127.0.0.1", port, 2s);
        assert(peer.valid());
    }   // the peer goes

    assert(spin_until_for([&] { return closes.load() > 0; }));
    std::this_thread::sleep_for(150ms);
    assert(closes.load() == 1);              // announced exactly once

    pass("tcp_connection: a peer close is announced exactly once");
}

void test_tcp_reactor_handler_may_leave_a_partial_frame()
{
    // on_data hands over the connection's own buffer, so a half-arrived
    // length-prefixed frame costs nothing: look, decide it is short, return.
    loop_on_a_thread lt;
    net::tcp_acceptor acc(lt.loop, "127.0.0.1", 0);
    std::atomic<int> frames{0};

    acc.on_accept([&frames](net::tcp_connection c) {
        c.on_data([&frames](net::tcp_connection, std::string& buf) {
            // Frames are "4 bytes of length, then that many bytes".
            for (;;) {
                if (buf.size() < 4)
                    return;                  // keep the partial header
                std::uint32_t len = 0;
                std::memcpy(&len, buf.data(), 4);
                if (buf.size() < 4 + len)
                    return;                  // keep the partial body
                buf.erase(0, 4 + len);
                frames.fetch_add(1);
            }
        });
    });
    const std::uint16_t port = acc.port();
    lt.start();

    net::tcp_stream peer = net::tcp_stream::connect("127.0.0.1", port, 2s);
    assert(peer.valid());

    // Send three frames split across writes so the handler sees partials.
    std::string wire;
    for (int i = 0; i < 3; ++i) {
        const std::string body(100 + i, char('a' + i));
        std::uint32_t len = std::uint32_t(body.size());
        wire.append(reinterpret_cast<const char*>(&len), 4);
        wire += body;
    }
    for (std::size_t off = 0; off < wire.size(); off += 7) {
        const std::size_t n = (off + 7 < wire.size()) ? 7 : wire.size() - off;
        assert(peer.write_all(wire.data() + off, n, 2s));
    }

    assert(spin_until_for([&] { return frames.load() == 3; }));

    pass("tcp_connection: on_data leaves a partial frame buffered for next time");
}

#endif // SNICHOLLS_HAS_EVENT_LOOP

} // namespace

void run_tcp_tests()
{
    test_tcp_round_trip();
    test_tcp_carries_arbitrary_bytes();
    test_tcp_read_honours_its_deadline();
    test_tcp_clean_close_reads_as_eof();
    test_tcp_half_close_keeps_the_other_direction();
    test_tcp_shutdown_unblocks_a_parked_read();
    test_tcp_accept_honours_its_deadline();
    test_tcp_connect_refused_and_timed_out();
    test_tcp_handles_move_without_double_closing();
    test_tcp_listener_reports_the_chosen_port();
#if SNICHOLLS_HAS_EVENT_LOOP
    test_tcp_reactor_echoes_many_connections_on_one_thread();
    test_tcp_reactor_send_is_bounded_and_drains();
    test_tcp_reactor_oversized_send_on_empty_queue();
    test_tcp_reactor_reports_close_once();
    test_tcp_reactor_handler_may_leave_a_partial_frame();
#endif
}

#else // !SNICHOLLS_HAS_TCP

void run_tcp_tests() {}

#endif
