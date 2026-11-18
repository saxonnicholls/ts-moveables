//
//  tests_h2_client.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - http::client over HTTP/2, part of `make test-client`. Three layers:
//
//    - h2_client_session alone, frame by frame: HPACK state kept across a cancelled stream, request bodies held
//      to the server's windows, a body that disagrees with its content-length.
//    - Against this library's own h2 server (h2c by prior knowledge): requests and bodies both ways larger than
//      every initial window, many requests multiplexed on one connection and answered out of order, and a mixed
//      listener that still speaks HTTP/1.1 to a client that did not ask for h2.
//    - Against a scripted server writing raw frames, for what ours never sends: trailers (gRPC's status), interim
//      heads, REFUSED_STREAM, GOAWAY with streams it never processed, resets, malformed heads, a stream that never
//      answers, and connection errors.
//

#include "client_harness.hpp"

#include <sys/time.h>

#include <map>
#include <mutex>

#include "http/h2_client.hpp"
#include "http/http2.hpp"

namespace {

using namespace client_tests;
namespace http = snicholls::http;
namespace hpack = snicholls::http::hpack;
using snicholls::http::h2_error;
using snicholls::http::h2_frame;

// ------------------------------------------------------------------ frames by hand

std::string frame(h2_frame type, std::uint8_t flags, std::uint32_t sid, const std::string& payload)
{
    std::string o;
    http::detail::h2_put_header(o, std::uint32_t(payload.size()), type, flags, sid);
    return o + payload;
}

std::string u32(std::uint32_t v)
{
    std::string o;
    http::detail::h2_put32(o, v);
    return o;
}

std::string block_of(const std::vector<hpack::field>& fields)
{
    hpack::encoder enc;
    std::string b;
    enc.encode(fields, b);
    return b;
}

std::string head(std::uint32_t sid, const std::vector<hpack::field>& fields, bool end_stream)
{
    return frame(h2_frame::headers, std::uint8_t(http::h2_flag_end_headers | (end_stream ? http::h2_flag_end_stream : 0)),
                 sid, block_of(fields));
}

std::string data(std::uint32_t sid, const std::string& body, bool end_stream)
{
    return frame(h2_frame::data, end_stream ? http::h2_flag_end_stream : 0, sid, body);
}

const std::string kServerSettings = std::string("\0\0\0\x04\0\0\0\0\0", 9);

struct parsed_frame {
    http::detail::h2_frame_header h;
    std::string payload;
};

std::vector<parsed_frame> frames_in(std::string bytes)
{
    std::vector<parsed_frame> out;
    if (bytes.compare(0, http::kH2PrefaceLen, http::kH2Preface) == 0)
        bytes.erase(0, http::kH2PrefaceLen);
    std::size_t off = 0;
    while (bytes.size() - off >= 9) {
        parsed_frame f;
        f.h = http::detail::h2_read_header(reinterpret_cast<const unsigned char*>(bytes.data()) + off);
        f.payload = bytes.substr(off + 9, f.h.length);
        off += 9 + f.h.length;
        out.push_back(std::move(f));
    }
    return out;
}

// ------------------------------------------------------------------ a scripted h2 server

// One accepted connection, spoken to frame by frame from a test's script
struct h2_peer {
    int fd;
    std::string buf;
    hpack::decoder dec;

    explicit h2_peer(int c) : fd(c)
    {
        timeval tv{5, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }

    bool fill()
    {
        char tmp[16384];
        const ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
        if (n <= 0) return false;
        buf.append(tmp, std::size_t(n));
        return true;
    }

    // The client's preface and SETTINGS, answered with ours and an ACK
    bool handshake()
    {
        while (buf.size() < http::kH2PrefaceLen)
            if (!fill()) return false;
        assert(buf.compare(0, http::kH2PrefaceLen, http::kH2Preface) == 0);
        buf.erase(0, http::kH2PrefaceLen);
        send_all(fd, kServerSettings + frame(h2_frame::settings, http::h2_flag_ack, 0, ""));
        return true;
    }

    bool next(parsed_frame& f)
    {
        while (buf.size() < 9 || buf.size() < 9 + http::detail::h2_read_header(
                                                      reinterpret_cast<const unsigned char*>(buf.data())).length)
            if (!fill()) return false;
        f.h = http::detail::h2_read_header(reinterpret_cast<const unsigned char*>(buf.data()));
        f.payload = buf.substr(9, f.h.length);
        buf.erase(0, 9 + f.h.length);
        return true;
    }

    struct request {
        std::uint32_t stream = 0;
        std::map<std::string, std::string> fields;
        std::string body;
    };

    // The next whole request (HEADERS through END_STREAM), skipping control frames; RST_STREAMs seen on the way
    // are recorded
    std::vector<std::pair<std::uint32_t, std::uint32_t>> resets;
    std::map<std::uint32_t, request> partial;
    bool next_request(request& r)
    {
        parsed_frame f;
        while (next(f)) {
            const auto type = h2_frame(f.h.type);
            if (type == h2_frame::rst_stream) {
                resets.emplace_back(f.h.stream, http::detail::h2_get32(reinterpret_cast<const unsigned char*>(f.payload.data())));
                continue;
            }
            if (type == h2_frame::headers) {
                assert(f.h.flags & http::h2_flag_end_headers);
                std::vector<hpack::field> fields;
                const auto* b = reinterpret_cast<const unsigned char*>(f.payload.data());
                assert(dec.decode(b, b + f.payload.size(), fields, 1 << 20, 1000) == hpack::status::ok);
                request& q = partial[f.h.stream];
                q.stream = f.h.stream;
                for (auto& [n, v] : fields) q.fields[n] = v;
            } else if (type == h2_frame::data) {
                partial[f.h.stream].body += f.payload;
            } else {
                continue;
            }
            if (f.h.flags & http::h2_flag_end_stream) {
                r = std::move(partial[f.h.stream]);
                partial.erase(f.h.stream);
                return true;
            }
        }
        return false;
    }

    // Read until the client closes (or the receive timeout passes), so the client ends the connection, not us
    void until_closed()
    {
        while (fill()) {
            parsed_frame f;
            while (buf.size() >= 9 && buf.size() >= 9 + http::detail::h2_read_header(
                                                            reinterpret_cast<const unsigned char*>(buf.data())).length) {
                f.h = http::detail::h2_read_header(reinterpret_cast<const unsigned char*>(buf.data()));
                if (h2_frame(f.h.type) == h2_frame::rst_stream)
                    resets.emplace_back(f.h.stream, http::detail::h2_get32(
                                                        reinterpret_cast<const unsigned char*>(buf.data()) + 9));
                buf.erase(0, 9 + f.h.length);
            }
        }
    }

    void send(const std::string& s) { send_all(fd, s); }
};

client_config h2c()
{
    client_config cfg;
    cfg.http2_prior_knowledge = true;
    return cfg;
}

// ------------------------------------------------------------------ the session alone

void test_session_keeps_hpack_state_across_a_cancelled_stream()
{
    http::h2_client_session s;
    s.start();
    const auto a = s.submit("GET", "http", "h", "/a", {}, {});
    const auto b = s.submit("GET", "http", "h", "/b", {}, {});
    assert(a == 1 && b == 3);
    s.cancel(a);
    (void)s.take_output();

    // Stream 1's head inserts "x-shared: yes" into the dynamic table; stream 3's head refers to it by index 62
    std::string block1 = "\x88";                                    // :status 200
    block1 += "\x40\x08x-shared\x03yes";                            // literal, incremental indexing, new name
    std::string block3 = "\x88\xbe";                                // :status 200, dynamic entry 62
    std::string in = kServerSettings + frame(h2_frame::headers, http::h2_flag_end_headers | http::h2_flag_end_stream, 1, block1) +
                     frame(h2_frame::headers, http::h2_flag_end_headers | http::h2_flag_end_stream, 3, block3);
    s.consume(in);
    assert(in.empty() && !s.failed());
    auto done = s.take_completions();
    assert(done.size() == 1 && done[0].stream == 3 && done[0].outcome == http::h2_outcome::response);
    assert(done[0].res.status == 200 && *done[0].res.header("x-shared") == "yes");
    pass("h2 session: a cancelled stream's header block is still decoded, so the next one's indexes hold");
}

void test_session_holds_request_bodies_to_the_windows()
{
    http::h2_client_session s;
    s.start();
    (void)s.take_output();
    const std::string body(100000, 'b');
    const auto id = s.submit("POST", "http", "h", "/up", {{"Content-Type", "x/y"}, {"Connection", "close"}}, body);
    std::size_t sent = 0;
    bool saw_type = false, saw_connection = false, saw_length = false;
    for (const auto& f : frames_in(s.take_output())) {
        if (h2_frame(f.h.type) == h2_frame::data) sent += f.h.length;
        if (h2_frame(f.h.type) == h2_frame::headers) {
            hpack::decoder d;
            std::vector<hpack::field> fields;
            const auto* p = reinterpret_cast<const unsigned char*>(f.payload.data());
            assert(d.decode(p, p + f.payload.size(), fields, 1 << 20, 100) == hpack::status::ok);
            for (auto& [n, v] : fields) {
                saw_type |= n == "content-type";
                saw_connection |= n == "connection";
                saw_length |= n == "content-length" && v == "100000";
            }
        }
    }
    assert(sent == 65535 && s.buffered_bytes() == 100000 - 65535);
    assert(saw_type && !saw_connection && saw_length);

    // The server opens both windows; the rest goes, and the last frame ends the stream
    std::string in = kServerSettings + frame(h2_frame::window_update, 0, 0, u32(1 << 20)) +
                     frame(h2_frame::window_update, 0, id, u32(1 << 20));
    s.consume(in);
    bool ended = false;
    for (const auto& f : frames_in(s.take_output()))
        if (h2_frame(f.h.type) == h2_frame::data) {
            sent += f.h.length;
            ended = (f.h.flags & http::h2_flag_end_stream) != 0;
        }
    assert(sent == 100000 && ended && s.buffered_bytes() == 0);
    pass("h2 session: a request body waits for the server's windows; names lowercased, Connection dropped");
}

void test_session_refuses_malformed_responses()
{
    http::h2_client_session s;
    s.start();
    const auto a = s.submit("GET", "http", "h", "/", {}, {});
    const auto b = s.submit("GET", "http", "h", "/", {}, {});
    const auto c = s.submit("GET", "http", "h", "/", {}, {});
    (void)s.take_output();
    std::string in = kServerSettings + head(a, {{":status", "200"}, {"content-length", "5"}}, false) +
                     data(a, "abc", true) +                                       // 3 bytes, not 5
                     head(b, {{":status", "200"}, {"Upper", "x"}}, true) +         // uppercase name
                     head(c, {{"x", "y"}}, true);                                  // no :status
    s.consume(in);
    auto done = s.take_completions();
    assert(done.size() == 3 && !s.failed());
    for (auto& d : done) assert(d.outcome == http::h2_outcome::malformed);
    std::size_t resets = 0;
    for (const auto& f : frames_in(s.take_output()))
        if (h2_frame(f.h.type) == h2_frame::rst_stream) ++resets;
    assert(resets == 3);
    pass("h2 session: a wrong content-length, an uppercase name and a missing :status reset only their stream");
}

// ------------------------------------------------------------------ against our own server

snicholls::http::server_config big_bodies()
{
    snicholls::http::server_config sc;
    sc.limits.max_body = 8u << 20;
    return sc;
}

struct live_h2_server : live_server {
    explicit live_h2_server(bool only = true) : live_server(big_bodies())
    {
        if (only)
            http::enable_http2_only(srv);
        else
            http::enable_http2(srv);
    }
};

void test_h2c_against_our_server()
{
    live_h2_server s;
    s.srv.get("/hello", [](const auto& req, auto res) { res.send(200, "text/plain", "hi " + req.header_or("user-agent")); });
    s.srv.head("/hello", [](const auto&, auto res) { res.send(200, "text/plain", "elided"); });
    s.srv.post("/echo", [](const auto& req, auto res) { res.send(200, "application/octet-stream", req.body); });
    s.srv.get("/big", [](const auto&, auto res) { res.send(200, "application/octet-stream", std::string(5u << 20, 'z')); });
    s.start();
    driver d(h2c());
    const std::string up(3u << 20, 'u');
    auto r = d.run({{"GET", s.url("/hello"), {}, {}, {}},
                    {"POST", s.url("/echo"), {{"Content-Type", "application/octet-stream"}}, up, {}},
                    {"GET", s.url("/big"), {}, {}, {}},
                    {"HEAD", s.url("/hello"), {}, {}, {}}});
    for (auto& x : r) assert(x.is_ok());
    assert(r[0]->status == 200 && r[0]->body == "hi ts-moveables" && r[0]->http_major == 2);
    assert(r[1]->body == up);
    assert(r[2]->body.size() == (5u << 20));
    assert(r[3]->status == 200 && r[3]->body.empty() && *r[3]->header("content-length") == "6");
    const auto st = d.http.stats();
    assert(st.connections_opened == 1 && st.http2_connections == 1 && st.responses == 4);
    pass("h2c: GET, HEAD, and 3 MiB up and 5 MiB down past every initial window, all on one connection");
}

void test_h2_multiplexes_out_of_order()
{
    live_h2_server s;
    std::mutex m;
    std::vector<std::thread> workers;
    s.srv.get("/n", [&](const auto& req, auto res) {
        const int n = std::stoi(req.query_param("i", "0"));
        std::lock_guard<std::mutex> lock(m);
        workers.emplace_back([n, res = std::move(res)]() mutable {
            std::this_thread::sleep_for(std::chrono::milliseconds(5 * (20 - n)));   // the first asked answers last
            res.send(200, "text/plain", std::to_string(n));
        });
    });
    s.start();
    std::vector<int> order;
    {
        driver d(h2c());
        std::vector<client_request> reqs;
        for (int i = 0; i < 20; ++i) reqs.push_back({"GET", s.url("/n?i=" + std::to_string(i)), {}, {}, {}});
        std::size_t done = 0;
        for (std::size_t i = 0; i < reqs.size(); ++i)
            d.http.request(reqs[i], [&, i](client_result r) {
                assert(r.is_ok() && r->body == std::to_string(i));
                order.push_back(int(i));
                ++done;
            });
        const auto until = std::chrono::steady_clock::now() + 10s;
        while (done < reqs.size() && std::chrono::steady_clock::now() < until) d.loop.run_once(10ms);
        assert(done == 20);
        assert(d.http.stats().connections_opened == 1);
    }
    for (auto& t : workers) t.join();
    assert(order.front() > order.back());
    pass("h2c: 20 requests share one connection and complete in the order the server answers them");
}

void test_mixed_listener_still_speaks_http1()
{
    live_h2_server s(false);
    s.srv.get("/v", [](const auto& req, auto res) { res.send(200, "text/plain", std::to_string(req.http_major)); });
    s.start();
    driver h1;
    auto a = h1.one({"GET", s.url("/v"), {}, {}, {}});
    assert(a.is_ok() && a->body == "1" && a->http_major == 1 && h1.http.stats().http2_connections == 0);
    driver h2(h2c());
    auto b = h2.one({"GET", s.url("/v"), {}, {}, {}});
    assert(b.is_ok() && b->body == "2" && b->http_major == 2);
    pass("h2c: a listener serving both answers HTTP/1.1 unless the client opens with the h2 preface");
}

// ------------------------------------------------------------------ against a scripted server

void test_trailers_and_interim_heads()
{
    raw_server srv([](int c) {
        h2_peer p(c);
        if (!p.handshake()) return;
        h2_peer::request q;
        if (!p.next_request(q)) return;
        assert(q.fields[":method"] == "POST" && q.fields[":path"] == "/pkg.Svc/Call" && q.fields["te"] == "trailers");
        assert(q.body == std::string("\0\0\0\0\x03" "abc", 8));
        p.send(head(q.stream, {{":status", "103"}, {"link", "</x>"}}, false) +
               head(q.stream, {{":status", "200"}, {"content-type", "application/grpc"}}, false) +
               data(q.stream, std::string("\0\0\0\0\x02" "ok", 7), false) +
               head(q.stream, {{"grpc-status", "0"}, {"grpc-message", "fine"}}, true));
        p.until_closed();
    });
    driver d(h2c());
    auto r = d.one({"POST",
                    srv.url("/pkg.Svc/Call"),
                    {{"Content-Type", "application/grpc"}, {"TE", "trailers"}},
                    std::string("\0\0\0\0\x03" "abc", 8),
                    {}});
    assert(r.is_ok() && r->status == 200 && r->body == std::string("\0\0\0\0\x02" "ok", 7));
    assert(r->headers.find("link") == nullptr);
    assert(*r->trailers.find("grpc-status") == "0" && *r->trailers.find("grpc-message") == "fine");
    pass("h2: a 103 is skipped, and the trailers after the body land in response::trailers (gRPC's status)");
}

void test_refused_and_goaway_streams_are_retried()
{
    std::atomic<int> conns{0};
    raw_server srv([&](int c) {
        h2_peer p(c);
        if (!p.handshake()) return;
        if (++conns == 1) {
            // Refuse the first, answer the retry; then GOAWAY naming stream 5 as the last processed, so 7 is not
            h2_peer::request q;
            assert(p.next_request(q) && q.stream == 1);
            p.send(frame(h2_frame::rst_stream, 0, 1, u32(std::uint32_t(h2_error::refused_stream))));
            assert(p.next_request(q) && q.stream == 3 && q.body == "once");
            p.send(head(3, {{":status", "201"}}, true));
            h2_peer::request a, b;
            assert(p.next_request(a) && p.next_request(b));
            p.send(frame(h2_frame::goaway, 0, 0, u32(5) + u32(0)) + head(5, {{":status", "200"}}, false) +
                   data(5, "five", true));
        } else {
            h2_peer::request q;
            assert(p.next_request(q) && q.fields[":path"] == "/seven");
            p.send(head(q.stream, {{":status", "200"}}, false) + data(q.stream, "seven", true));
        }
        p.until_closed();
    });
    driver d(h2c());
    auto first = d.one({"POST", srv.url("/once"), {}, "once", {}});
    assert(first.is_ok() && first->status == 201);
    auto r = d.run({{"GET", srv.url("/five"), {}, {}, {}}, {"POST", srv.url("/seven"), {}, "x", {}}});
    assert(r[0].is_ok() && r[0]->body == "five");
    assert(r[1].is_ok() && r[1]->body == "seven");
    const auto st = d.http.stats();
    assert(st.retries == 2 && st.connections_opened == 2);
    pass("h2: REFUSED_STREAM and streams above GOAWAY's last are retried - even a POST - and the rest finish");
}

void test_resets_and_malformed_heads_fail_one_stream()
{
    raw_server srv([](int c) {
        h2_peer p(c);
        if (!p.handshake()) return;
        for (int i = 0; i < 3; ++i) {
            h2_peer::request q;
            if (!p.next_request(q)) return;
            const std::string path = q.fields[":path"];
            if (path == "/reset")
                p.send(frame(h2_frame::rst_stream, 0, q.stream, u32(std::uint32_t(h2_error::internal_error))));
            else if (path == "/bad")
                p.send(head(q.stream, {{":status", "200"}, {"transfer-encoding", "chunked"}}, true));
            else
                p.send(head(q.stream, {{":status", "200"}}, false) + data(q.stream, "fine", true));
        }
        p.until_closed();
    });
    driver d(h2c());
    auto a = d.one({"GET", srv.url("/reset"), {}, {}, {}});
    assert(a.is_err() && code_of(a) == client_errc::stream_reset);
    auto b = d.one({"GET", srv.url("/bad"), {}, {}, {}});
    assert(b.is_err() && code_of(b) == client_errc::protocol_error);
    auto c = d.one({"GET", srv.url("/ok"), {}, {}, {}});
    assert(c.is_ok() && c->body == "fine");
    assert(d.http.stats().connections_opened == 1);
    pass("h2: a reset and a malformed head fail their own request; the connection carries on");
}

void test_a_timeout_cancels_only_its_stream()
{
    std::vector<std::pair<std::uint32_t, std::uint32_t>> resets;
    std::mutex m;
    {
        raw_server srv([&](int c) {
            h2_peer p(c);
            if (!p.handshake()) return;
            h2_peer::request slow, fast;
            if (!p.next_request(slow) || !p.next_request(fast)) return;
            p.send(head(fast.stream, {{":status", "200"}}, false) + data(fast.stream, "fast", true));
            p.until_closed();
            std::lock_guard<std::mutex> lock(m);
            resets = p.resets;
        });
        driver d(h2c());
        auto r = d.run({{"GET", srv.url("/slow"), {}, {}, std::chrono::milliseconds(300)},
                        {"GET", srv.url("/fast"), {}, {}, {}}});
        assert(r[0].is_err() && code_of(r[0]) == client_errc::timed_out);
        assert(r[1].is_ok() && r[1]->body == "fast");
    }
    std::lock_guard<std::mutex> lock(m);
    assert(!resets.empty() && resets[0].first == 1 && resets[0].second == std::uint32_t(h2_error::cancel));
    pass("h2: a timeout sends RST_STREAM CANCEL for its own stream and leaves the other one alone");
}

void test_connection_errors_fail_everything_on_it()
{
    raw_server srv([](int c) {
        h2_peer p(c);
        if (!p.handshake()) return;
        h2_peer::request q;
        if (!p.next_request(q)) return;
        // PUSH_PROMISE, which our SETTINGS refused
        p.send(frame(h2_frame::push_promise, http::h2_flag_end_headers, q.stream, u32(2) + "\x82"));
        p.until_closed();
    });
    driver d(h2c());
    auto r = d.one({"POST", srv.url("/x"), {}, "body", {}});
    assert(r.is_err() && code_of(r) == client_errc::protocol_error);

    raw_server http1([](int c) {
        read_request(c);
        send_all(c, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    });
    auto e = d.one({"GET", http1.url("/"), {}, {}, {}});
    assert(e.is_err());
    pass("h2: a PUSH_PROMISE ends the connection with its requests failed; an HTTP/1.1-only server fails h2c");
}

} // namespace

void run_h2_client_tests()
{
    test_session_keeps_hpack_state_across_a_cancelled_stream();
    test_session_holds_request_bodies_to_the_windows();
    test_session_refuses_malformed_responses();
    test_h2c_against_our_server();
    test_h2_multiplexes_out_of_order();
    test_mixed_listener_still_speaks_http1();
    test_trailers_and_interim_heads();
    test_refused_and_goaway_streams_are_retried();
    test_resets_and_malformed_heads_fail_one_stream();
    test_a_timeout_cancels_only_its_stream();
    test_connection_errors_fail_everything_on_it();
}
