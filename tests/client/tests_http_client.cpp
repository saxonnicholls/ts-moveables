//
//  tests_http_client.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - http::client, its own program (C++20, like the client), run by `make test-client`.
//  Against this library's http::server for the ordinary paths, and against a scripted raw-socket server for what a
//  well-behaved server never does: chunked bodies with trailers, interim 1xx responses, bodies that run to the
//  close, a server that never answers, a pooled connection the server closed while it sat idle, and garbage.
//

#include "../test_helpers.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "http/client.hpp"
#include "http/server.hpp"
#include "net/url.hpp"

namespace {

using namespace std::chrono_literals;
using snicholls::event_loop;
using snicholls::http::client;
using snicholls::http::client_config;
using snicholls::http::client_errc;
using snicholls::http::client_request;
using snicholls::http::client_result;

// ------------------------------------------------------------------ helpers

// A server of our own on a thread
struct live_server {
    snicholls::http::server srv;
    std::uint16_t port = 0;
    std::thread th;
    void start()
    {
        port = srv.listen("127.0.0.1", 0);
        th = std::thread([this] { srv.run(); });
        while (!srv.running()) std::this_thread::yield();
    }
    ~live_server()
    {
        srv.stop();
        if (th.joinable()) th.join();
    }
    std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port) + path; }
};

// A raw TCP server: each accepted connection is handed to `script` on a thread of its own
struct raw_server {
    int fd = -1;
    std::uint16_t port = 0;
    std::thread th;
    std::vector<std::thread> conns;
    std::atomic<bool> stop{false};
    std::atomic<int> accepted{0};

    explicit raw_server(std::function<void(int)> script)
    {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        const int on = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        assert(::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
        assert(::listen(fd, 16) == 0);
        socklen_t len = sizeof a;
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        th = std::thread([this, script] {
            for (;;) {
                const int c = ::accept(fd, nullptr, nullptr);
                if (c < 0 || stop.load()) {
                    if (c >= 0) ::close(c);
                    return;
                }
                ++accepted;
                conns.emplace_back([c, script] {
                    script(c);
                    ::close(c);
                });
            }
        });
    }
    ~raw_server()
    {
        stop = true;
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
        // Wake a blocked accept on platforms where closing the socket does not
        const int s = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
        ::close(s);
        th.join();
        for (auto& t : conns) t.join();
    }
    std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port) + path; }
};

// Read one request head (and a Content-Length body) from a raw connection; empty when the client went away
std::string read_request(int c)
{
    std::string got;
    char buf[4096];
    while (got.find("\r\n\r\n") == std::string::npos) {
        const ssize_t n = ::recv(c, buf, sizeof buf, 0);
        if (n <= 0) return {};
        got.append(buf, std::size_t(n));
    }
    const std::size_t cl = got.find("Content-Length: ");
    if (cl != std::string::npos) {
        const std::size_t want = std::stoul(got.substr(cl + 16));
        const std::size_t have = got.size() - (got.find("\r\n\r\n") + 4);
        for (std::size_t left = want - have; left > 0;) {
            const ssize_t n = ::recv(c, buf, sizeof buf, 0);
            if (n <= 0) break;
            got.append(buf, std::size_t(n));
            left -= std::min<std::size_t>(left, std::size_t(n));
        }
    }
    return got;
}

void send_all(int c, const std::string& s)
{
    std::size_t off = 0;
    while (off < s.size()) {
        const ssize_t n = ::send(c, s.data() + off, s.size() - off, 0);
        if (n <= 0) return;
        off += std::size_t(n);
    }
}

// Run requests on a loop on this thread until every callback has run (or 10 s pass)
struct driver {
    event_loop loop;
    client http;
    explicit driver(client_config cfg = {}) : http(loop, std::move(cfg)) {}
    std::vector<client_result> run(std::vector<client_request> reqs)
    {
        std::vector<std::optional<client_result>> out(reqs.size());
        std::size_t done = 0;
        for (std::size_t i = 0; i < reqs.size(); ++i)
            http.request(reqs[i], [&out, &done, i](client_result r) {
                out[i].emplace(std::move(r));
                ++done;
            });
        const auto until = std::chrono::steady_clock::now() + 10s;
        while (done < reqs.size() && std::chrono::steady_clock::now() < until) loop.run_once(10ms);
        assert(done == reqs.size());
        std::vector<client_result> results;
        for (auto& o : out) results.push_back(std::move(*o));
        return results;
    }
    client_result one(client_request r) { return std::move(run({std::move(r)})[0]); }
};

client_errc code_of(const client_result& r) { return r.unwrap_err_unchecked().code(); }

// ------------------------------------------------------------------ the ordinary paths

void test_url_parsing()
{
    snicholls::net::url u;
    assert(snicholls::net::parse_url("http://example.com", u) && u.host == "example.com" && u.port == "80" &&
           u.target == "/" && !u.secure && u.authority() == "example.com");
    assert(snicholls::net::parse_url("HTTPS://h:8443/a/b?c=d#frag", u) && u.scheme == "https" && u.port == "8443" &&
           u.target == "/a/b?c=d" && u.secure && u.authority() == "h:8443");
    assert(snicholls::net::parse_url("http://[::1]:9000?q=1", u) && u.host == "::1" && u.port == "9000" &&
           u.target == "/?q=1" && u.authority() == "[::1]:9000");
    for (const char* bad : {"ftp://h/", "http://", "http://user:pw@h/", "http://h:x/", "h/path", "http://h/a b",
                            "http://[::1/"})
        assert(!snicholls::net::parse_url(bad, u));
    pass("url: schemes, default ports, IPv6, targets; credentials, spaces and junk refused");
}

void test_get_and_post_against_our_server()
{
    live_server s;
    s.srv.get("/hello", [](const auto& req, auto res) {
        res.send(200, "text/plain", "hi " + req.header_or("user-agent"));
    });
    s.srv.post("/echo", [](const auto& req, auto res) { res.send(201, "application/json", req.body); });
    s.start();

    driver d;
    const auto r = d.one({"GET", s.url("/hello"), {}, {}, {}});
    assert(r.is_ok() && r->status == 200 && r->body == "hi ts-moveables");
    assert(*r->header("content-type") == "text/plain");

    const auto p = d.one({"POST", s.url("/echo"), {{"Content-Type", "application/json"}}, "{\"a\":1}", {}});
    assert(p.is_ok() && p->status == 201 && p->body == "{\"a\":1}");

    const auto missing = d.one({"GET", s.url("/nope"), {}, {}, {}});
    assert(missing.is_ok() && missing->status == 404);   // an HTTP error status is a response, not a failure
    pass("http client: GET and POST against http::server; a 404 is a response");
}

void test_keep_alive_reuses_one_connection()
{
    live_server s;
    s.srv.get("/n", [](const auto&, auto res) { res.send(200, "text/plain", "x"); });
    s.start();
    driver d;
    for (int i = 0; i < 5; ++i) assert(d.one({"GET", s.url("/n"), {}, {}, {}}).is_ok());
    const auto st = d.http.stats();
    assert(st.connections_opened == 1 && st.reused == 4 && st.responses == 5);
    assert(d.http.idle_connections() == 1);
    pass("http client: five sequential requests share one keep-alive connection");
}

void test_connections_per_host_are_bounded()
{
    std::atomic<int> concurrent{0}, peak{0};
    raw_server s([&](int c) {
        for (;;) {
            if (read_request(c).empty()) return;
            const int now = ++concurrent;
            int p = peak.load();
            while (now > p && !peak.compare_exchange_weak(p, now)) {}
            std::this_thread::sleep_for(20ms);
            --concurrent;
            send_all(c, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
        }
    });
    client_config cfg;
    cfg.max_connections_per_host = 2;
    driver d(cfg);
    std::vector<client_request> reqs(10, client_request{"GET", s.url("/slow"), {}, {}, {}});
    for (const auto& r : d.run(reqs)) assert(r.is_ok() && r->body == "ok");
    assert(peak.load() <= 2 && s.accepted.load() == 2);
    assert(d.http.stats().connections_opened == 2);
    pass("http client: ten requests, at most two connections, the rest queued");
}

void test_fetch_blocks_for_one_answer()
{
    live_server s;
    s.srv.get("/f", [](const auto&, auto res) { res.send(200, "text/plain", "fetched"); });
    s.start();
    const auto r = snicholls::http::fetch({"GET", s.url("/f"), {}, {}, {}});
    assert(r.is_ok() && r->body == "fetched");
    pass("http client: fetch() runs one request on a loop of its own");
}

// ------------------------------------------------------------------ what the server can do to us

void test_framing_from_a_raw_server()
{
    raw_server s([](int c) {
        for (;;) {
            const std::string req = read_request(c);
            if (req.empty()) return;
            if (req.find("GET /chunked") == 0)
                send_all(c, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                            "5\r\nhello\r\n6\r\n world\r\n0\r\ngrpc-status: 0\r\ngrpc-message: fine\r\n\r\n");
            else if (req.find("HEAD /") == 0)
                send_all(c, "HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\n");     // HEAD: no body follows
            else if (req.find("GET /nocontent") == 0)
                send_all(c, "HTTP/1.1 204 No Content\r\n\r\n");
            else if (req.find("GET /interim") == 0)
                send_all(c, "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\nLink: </a>\r\n\r\n"
                            "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nreal");
            else if (req.find("GET /toclose") == 0) {
                send_all(c, "HTTP/1.1 200 OK\r\n\r\nall of this until the close");
                return;
            }
        }
    });
    driver d;
    const auto chunked = d.one({"GET", s.url("/chunked"), {}, {}, {}});
    assert(chunked.is_ok() && chunked->body == "hello world");
    assert(*chunked->trailers.find("grpc-status") == "0" && *chunked->trailers.find("grpc-message") == "fine");
    assert(chunked->headers.find("grpc-status") == nullptr);   // trailers never leak into the headers

    const auto head = d.one({"HEAD", s.url("/"), {}, {}, {}});
    assert(head.is_ok() && head->status == 200 && head->body.empty());
    const auto none = d.one({"GET", s.url("/nocontent"), {}, {}, {}});
    assert(none.is_ok() && none->status == 204);
    const auto interim = d.one({"GET", s.url("/interim"), {}, {}, {}});
    assert(interim.is_ok() && interim->status == 200 && interim->body == "real");
    assert(d.http.stats().connections_opened == 1);   // all of that on one connection

    const auto toclose = d.one({"GET", s.url("/toclose"), {}, {}, {}});
    assert(toclose.is_ok() && toclose->body == "all of this until the close" && !toclose->keep_alive);
    pass("http client: chunked with trailers, HEAD, 204, interim 1xx, a body that runs to the close");
}

void test_a_stale_pooled_connection_is_retried_once_for_idempotent_methods()
{
    // Each connection answers one request and then closes without saying so (no Connection: close)
    raw_server s([](int c) {
        if (read_request(c).empty()) return;
        send_all(c, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
        std::this_thread::sleep_for(50ms);   // long enough for the client to pool it
    });
    driver d;
    assert(d.one({"GET", s.url("/a"), {}, {}, {}}).is_ok());
    std::this_thread::sleep_for(150ms);      // the server has closed it by now; the client does not know
    const auto again = d.one({"GET", s.url("/b"), {}, {}, {}});
    assert(again.is_ok() && again->body == "ok");
    assert(d.http.stats().retries == 1);

    assert(d.one({"GET", s.url("/c"), {}, {}, {}}).is_ok());
    std::this_thread::sleep_for(150ms);
    const auto post = d.one({"POST", s.url("/d"), {}, "x", {}});
    assert(post.is_err() && code_of(post) == client_errc::connection_closed);   // a POST is never resent
    pass("http client: a dead pooled connection is retried once for GET, never for POST");
}

void test_timeouts_and_failures()
{
    // A server that accepts and says nothing
    raw_server silent([](int c) {
        read_request(c);
        std::this_thread::sleep_for(1500ms);
    });
    driver d;
    const auto t0 = std::chrono::steady_clock::now();
    const auto late = d.one({"GET", silent.url("/"), {}, {}, 200ms});
    const auto took = std::chrono::steady_clock::now() - t0;
    assert(late.is_err() && code_of(late) == client_errc::timed_out);
    assert(took >= 190ms && took < 1000ms);

    raw_server garbage([](int c) {
        read_request(c);
        send_all(c, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello!");
    });
    const auto bad = d.one({"GET", garbage.url("/"), {}, {}, {}});
    assert(bad.is_err() && code_of(bad) == client_errc::protocol_error);

    // A port nothing listens on: bind one, close it
    std::uint16_t dead_port = 0;
    {
        raw_server tmp([](int) {});
        dead_port = tmp.port;
    }
    const auto refused = d.one({"GET", "http://127.0.0.1:" + std::to_string(dead_port) + "/", {}, {}, {}});
    assert(refused.is_err() && code_of(refused) == client_errc::connect_failed);

    assert(code_of(d.one({"GET", "ftp://x/", {}, {}, {}})) == client_errc::bad_url);
    assert(code_of(d.one({"GET", "https://example.com/", {}, {}, {}})) == client_errc::tls_unavailable);
    assert(code_of(d.one({"GET", "http://127.0.0.1:1/", {{"X-Evil", "a\r\nInjected: 1"}}, {}, {}})) ==
           client_errc::bad_request);

    client_config small;
    small.limits.max_body = 4;
    driver d2(small);
    raw_server big([](int c) {
        read_request(c);
        send_all(c, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n" + std::string(100, 'x'));
    });
    const auto too_big = d2.one({"GET", big.url("/"), {}, {}, {}});
    assert(too_big.is_err() && code_of(too_big) == client_errc::body_too_large);
    pass("http client: timeout, malformed response, refused, bad URL, no TLS, header injection, body limit");
}

void test_destroying_the_client_cancels_what_is_outstanding()
{
    raw_server silent([](int c) {
        read_request(c);
        std::this_thread::sleep_for(500ms);
    });
    event_loop loop;
    std::vector<client_errc> codes;
    {
        client_config cfg;
        cfg.max_connections_per_host = 1;
        client http(loop, cfg);
        for (int i = 0; i < 3; ++i)
            http.get(silent.url("/"), [&](client_result r) { codes.push_back(code_of(r)); });
        for (int i = 0; i < 20; ++i) loop.run_once(5ms);   // one in flight, two queued
    }
    for (int i = 0; i < 10 && codes.size() < 3; ++i) loop.run_once(5ms);
    assert(codes.size() == 3);
    for (const auto c : codes) assert(c == client_errc::cancelled);
    pass("http client: destroying it answers every outstanding request with cancelled");
}

} // namespace

int main()
{
    test_url_parsing();
    test_get_and_post_against_our_server();
    test_keep_alive_reuses_one_connection();
    test_connections_per_host_are_bounded();
    test_fetch_blocks_for_one_answer();
    test_framing_from_a_raw_server();
    test_a_stale_pooled_connection_is_retried_once_for_idempotent_methods();
    test_timeouts_and_failures();
    test_destroying_the_client_cancels_what_is_outstanding();
    std::cout << "\nhttp client: all " << tests_run << " tests passed\n";
    return 0;
}
