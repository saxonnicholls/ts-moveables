//
//  tests_http_server.cpp
//  TSMoveables
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - unit tests for the HTTP server (phase 1)
//
//  Two halves. The first tortures the parser without a socket in sight: every
//  documented request-smuggling vector is asserted *rejected*, and every input
//  is replayed one byte at a time to prove no fragment boundary changes the
//  outcome. The second runs a real server on loopback and drives it with a
//  blocking client - keep-alive, pipelining, chunked bodies, 100-continue,
//  async responses from other threads, and a response too large for one write.
//

#include "test_helpers.hpp"

#include "../TSMoveables/http_server.hpp"
#include "../TSMoveables/moveable_mutex.hpp"

#if SNICHOLLS_HAS_HTTP_SERVER

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace snicholls;
using namespace snicholls::http;
using namespace std::chrono_literals;

namespace {

// ============================================================ parser torture

// Feed the whole buffer in one call
request_parser::status parse_all(request_parser& p, const std::string& in, std::size_t& used)
{
    return p.parse(in.data(), in.size(), used);
}

// Feed one byte at a time - no split may change the verdict
request_parser::status parse_dribbled(request_parser& p, const std::string& in)
{
    std::string buf;
    auto st = request_parser::status::need_more;
    for (std::size_t i = 0; i < in.size(); ++i) {
        buf.push_back(in[i]);
        std::size_t used = 0;
        st = p.parse(buf.data(), buf.size(), used);
        buf.erase(0, used);
        if (st != request_parser::status::need_more)
            break;
    }
    return st;
}

// Both feed orders must agree, which is the whole point of an incremental parser
void expect_both(const std::string& in, request_parser::status want, int want_status = 0)
{
    request_parser whole;
    std::size_t used = 0;
    const auto a = parse_all(whole, in, used);
    assert(a == want);
    if (want_status)
        assert(whole.error_status() == want_status);

    request_parser drip;
    const auto b = parse_dribbled(drip, in);
    assert(b == want);
    if (want_status)
        assert(drip.error_status() == want_status);
}

void expect_rejected(const std::string& in, int status)
{
    expect_both(in, request_parser::status::failed, status);
}

void expect_accepted(const std::string& in)
{
    expect_both(in, request_parser::status::have_request);
}

const char* const kHost = "Host: example.com\r\n";

void test_parser_basics()
{
    request_parser p;
    const std::string in = "GET /a/b?x=1&y=two%20words HTTP/1.1\r\n"
                           "Host: example.com\r\n"
                           "User-Agent:   curl/8   \r\n"
                           "Accept: */*\r\n"
                           "\r\n";
    std::size_t used = 0;
    assert(parse_all(p, in, used) == request_parser::status::have_request);
    assert(used == in.size());

    const request& r = p.message();
    assert(r.method == method::get);
    assert(r.method_text == "GET");
    assert(r.path == "/a/b");
    assert(r.query == "x=1&y=two%20words");
    assert(r.http_major == 1 && r.http_minor == 1);
    assert(r.keep_alive);
    assert(r.headers.size() == 3);
    assert(*r.header("HOST") == "example.com");         // lookup is case-insensitive
    assert(*r.header("user-agent") == "curl/8");        // value is OWS-trimmed
    assert(r.query_param("x") == "1");
    assert(r.query_param("y") == "two words");          // percent-decoded
    assert(r.query_param("absent", "dflt") == "dflt");

    pass("http parser: request line, headers, query");
}

void test_parser_fragmentation()
{
    // The same request, delivered one byte at a time, must parse identically
    const std::string in = "POST /submit HTTP/1.1\r\nHost: h\r\nContent-Length: 11\r\n\r\nhello world";
    request_parser p;
    assert(parse_dribbled(p, in) == request_parser::status::have_request);
    assert(p.message().body == "hello world");
    assert(p.message().method == method::post);

    // And a pipelined pair drains one at a time from a single buffer
    const std::string two = std::string("GET /1 HTTP/1.1\r\n") + kHost + "\r\n" +
                            "GET /2 HTTP/1.1\r\n" + kHost + "\r\n";
    request_parser q;
    std::string buf = two;
    std::size_t used = 0;
    assert(q.parse(buf.data(), buf.size(), used) == request_parser::status::have_request);
    assert(q.message().path == "/1");
    buf.erase(0, used);
    q.reset();
    assert(q.parse(buf.data(), buf.size(), used) == request_parser::status::have_request);
    assert(q.message().path == "/2");

    pass("http parser: byte-by-byte and pipelined delivery");
}

void test_parser_smuggling_defences()
{
    // Every one of these is a documented request-smuggling vector. Being
    // liberal here is how a server becomes a gadget in someone else's chain.
    expect_rejected(std::string("POST / HTTP/1.1\r\n") + kHost +
                    "Content-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", 400);

    expect_rejected(std::string("POST / HTTP/1.1\r\n") + kHost +
                    "Content-Length: 5\r\nContent-Length: 6\r\n\r\nhello", 400);

    expect_rejected(std::string("POST / HTTP/1.1\r\n") + kHost +
                    "Content-Length: 5, 6\r\n\r\nhello", 400);

    expect_rejected(std::string("GET / HTTP/1.1\r\n") + kHost +
                    "Content-Length : 5\r\n\r\n", 400);              // space before colon

    expect_rejected(std::string("GET / HTTP/1.1\r\n") + kHost +
                    "X-Long: a\r\n b\r\n\r\n", 400);                 // obsolete line folding

    expect_rejected("GET / HTTP/1.1\nHost: h\r\n\r\n", 400);         // bare LF in request line
    expect_rejected(std::string("GET / HTTP/1.1\r\n") + kHost +
                    "X-Bad: v\n\r\n", 400);                          // bare LF in a header

    expect_rejected(std::string("GET / HTTP/1.1\r\n") + kHost +
                    "X-Bad: a\rb\r\n\r\n", 400);                     // stray CR inside a value

    // No Host, or two of them: both are desync material
    expect_rejected("GET / HTTP/1.1\r\n\r\n", 400);
    expect_rejected("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400);

    // Identical duplicates and identical list values are legal
    expect_accepted(std::string("POST / HTTP/1.1\r\n") + kHost +
                    "Content-Length: 5\r\nContent-Length: 5\r\n\r\nhello");
    expect_accepted(std::string("POST / HTTP/1.1\r\n") + kHost +
                    "Content-Length: 5, 5\r\n\r\nhello");

    pass("http parser: smuggling vectors rejected");
}

void test_parser_transfer_encoding()
{
    // chunked must be the final coding
    expect_rejected(std::string("POST / HTTP/1.1\r\n") + kHost +
                    "Transfer-Encoding: gzip\r\n\r\n", 501);
    expect_rejected(std::string("POST / HTTP/1.1\r\n") + kHost +
                    "Transfer-Encoding: chunked, gzip\r\n\r\n0\r\n\r\n", 501);
    expect_rejected(std::string("POST / HTTP/1.1\r\n") + kHost +
                    "Transfer-Encoding: chunked, chunked\r\n\r\n0\r\n\r\n", 400);

    // A chunked body with extensions and trailers reassembles exactly
    request_parser p;
    const std::string in = std::string("POST /up HTTP/1.1\r\n") + kHost +
                           "Transfer-Encoding: gzip, chunked\r\n\r\n"
                           "5;ext=1\r\nhello\r\n"
                           "6\r\n world\r\n"
                           "0\r\n"
                           "X-Checksum: abc\r\n"
                           "\r\n";
    assert(parse_dribbled(p, in) == request_parser::status::have_request);
    assert(p.message().body == "hello world");
    // Trailers are validated and discarded, never merged into the header set
    assert(!p.message().headers.has("x-checksum"));

    expect_rejected(std::string("POST / HTTP/1.1\r\n") + kHost +
                    "Transfer-Encoding: chunked\r\n\r\n5\r\nhelloXX", 400);   // bad terminator
    expect_rejected(std::string("POST / HTTP/1.1\r\n") + kHost +
                    "Transfer-Encoding: chunked\r\n\r\nzz\r\n", 400);         // not hex
    expect_rejected(std::string("POST / HTTP/1.1\r\n") + kHost +
                    "Transfer-Encoding: chunked\r\n\r\nffffffffffffffffff\r\n", 413);

    pass("http parser: chunked framing");
}

void test_parser_limits_and_versions()
{
    parse_limits tight;
    tight.max_request_line = 64;
    tight.max_headers = 3;
    tight.max_header_bytes = 128;
    tight.max_body = 16;

    {
        request_parser p(tight);
        const std::string in = "GET /" + std::string(200, 'x') + " HTTP/1.1\r\n\r\n";
        std::size_t used = 0;
        assert(parse_all(p, in, used) == request_parser::status::failed);
        assert(p.error_status() == 414);
    }
    {
        request_parser p(tight);
        std::string in = std::string("GET / HTTP/1.1\r\n") + kHost;
        for (int i = 0; i < 8; ++i)
            in += "X-H" + std::to_string(i) + ": v\r\n";
        in += "\r\n";
        std::size_t used = 0;
        assert(parse_all(p, in, used) == request_parser::status::failed);
        assert(p.error_status() == 431);
    }
    {
        request_parser p(tight);
        const std::string in = std::string("POST / HTTP/1.1\r\n") + kHost +
                               "Content-Length: 999\r\n\r\n";
        std::size_t used = 0;
        assert(parse_all(p, in, used) == request_parser::status::failed);
        assert(p.error_status() == 413);
    }

    // Versions: 2.x is 505, anything malformed is 400
    expect_rejected(std::string("GET / HTTP/2.0\r\n") + kHost + "\r\n", 505);
    expect_rejected(std::string("GET / HTTP/1.11\r\n") + kHost + "\r\n", 400);
    expect_rejected(std::string("GET / HTTPS/1.1\r\n") + kHost + "\r\n", 400);
    expect_rejected("GET /\r\n\r\n", 400);
    expect_rejected("GE\x01T / HTTP/1.1\r\nHost: h\r\n\r\n", 400);
    expect_rejected("GET  / HTTP/1.1\r\nHost: h\r\n\r\n", 400);      // extra space: ambiguous target

    // HTTP/1.0 needs no Host, and defaults to close
    {
        request_parser p;
        std::size_t used = 0;
        const std::string in = "GET / HTTP/1.0\r\n\r\n";
        assert(parse_all(p, in, used) == request_parser::status::have_request);
        assert(!p.message().keep_alive);
    }
    {
        request_parser p;
        std::size_t used = 0;
        const std::string in = "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n";
        assert(parse_all(p, in, used) == request_parser::status::have_request);
        assert(p.message().keep_alive);
    }
    {
        request_parser p;
        std::size_t used = 0;
        const std::string in = std::string("GET / HTTP/1.1\r\n") + kHost + "Connection: close\r\n\r\n";
        assert(parse_all(p, in, used) == request_parser::status::have_request);
        assert(!p.message().keep_alive);
    }

    pass("http parser: limits, versions, keep-alive defaults");
}

void test_parser_targets()
{
    {   // percent-decoding, and a NUL smuggled through %00
        request_parser p;
        std::size_t used = 0;
        const std::string in = std::string("GET /a%2Fb/c%41 HTTP/1.1\r\n") + kHost + "\r\n";
        assert(parse_all(p, in, used) == request_parser::status::have_request);
        assert(p.message().path == "/a/b/cA");
        assert(p.message().target == "/a%2Fb/c%41");    // the raw target survives too
    }
    expect_rejected(std::string("GET /a%zz HTTP/1.1\r\n") + kHost + "\r\n", 400);
    expect_rejected(std::string("GET /a%4 HTTP/1.1\r\n") + kHost + "\r\n", 400);
    expect_rejected(std::string("GET /a%00b HTTP/1.1\r\n") + kHost + "\r\n", 400);

    {   // absolute-form is accepted and reduced to a path
        request_parser p;
        std::size_t used = 0;
        const std::string in = "GET http://example.com/x?y=1 HTTP/1.1\r\nHost: example.com\r\n\r\n";
        assert(parse_all(p, in, used) == request_parser::status::have_request);
        assert(p.message().path == "/x");
        assert(p.message().query == "y=1");
    }
    {   // asterisk-form
        request_parser p;
        std::size_t used = 0;
        const std::string in = std::string("OPTIONS * HTTP/1.1\r\n") + kHost + "\r\n";
        assert(parse_all(p, in, used) == request_parser::status::have_request);
        assert(p.message().path == "*");
        assert(p.message().method == method::options);
    }
    expect_rejected(std::string("GET nonsense HTTP/1.1\r\n") + kHost + "\r\n", 400);

    {   // Expect: 100-continue is reported before the body arrives
        request_parser p;
        std::size_t used = 0;
        const std::string in = std::string("POST / HTTP/1.1\r\n") + kHost +
                               "Content-Length: 4\r\nExpect: 100-continue\r\n\r\n";
        assert(parse_all(p, in, used) == request_parser::status::need_more);
        assert(p.headers_done());
        assert(p.expects_continue());
    }

    pass("http parser: request-target forms and 100-continue");
}

// ================================================================= end to end

// A deliberately dumb blocking client: the point is to exercise the server,
// not to be clever
class client {
public:
    explicit client(std::uint16_t port)
    {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        assert(fd_ >= 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        assert(::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
        timeval tv{};
        tv.tv_sec = 5;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }
    client(const client&) = delete;
    client& operator=(const client&) = delete;
    ~client() { close(); }

    void close()
    {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    void send_raw(const std::string& s)
    {
        std::size_t off = 0;
        while (off < s.size()) {
            const ssize_t n = ::send(fd_, s.data() + off, s.size() - off, 0);
            if (n <= 0)
                return;
            off += std::size_t(n);
        }
    }

    // Read exactly one response; returns false on timeout or a dead peer.
    // no_body covers the cases where Content-Length describes a body that is
    // deliberately not sent: a response to HEAD, and any interim 1xx.
    bool read_response(int& status, std::string& head, std::string& body, bool no_body = false)
    {
        for (;;) {
            const std::size_t hend = buf_.find("\r\n\r\n");
            if (hend != std::string::npos) {
                head = buf_.substr(0, hend + 4);
                status = std::atoi(head.c_str() + 9);
                std::size_t want = 0;
                const std::size_t cl = ilower(head).find("\r\ncontent-length:");
                if (cl != std::string::npos)
                    want = std::size_t(std::atoll(head.c_str() + cl + 17));
                if (no_body || (status >= 100 && status < 200))
                    want = 0;
                if (buf_.size() >= hend + 4 + want) {
                    body = buf_.substr(hend + 4, want);
                    buf_.erase(0, hend + 4 + want);
                    return true;
                }
            }
            char tmp[8192];
            const ssize_t n = ::recv(fd_, tmp, sizeof tmp, 0);
            if (n <= 0)
                return false;
            buf_.append(tmp, std::size_t(n));
        }
    }

    // Read one chunked response, de-framing the body
    bool read_chunked(int& status, std::string& head, std::string& body)
    {
        body.clear();
        while (buf_.find("\r\n\r\n") == std::string::npos)
            if (!pump())
                return false;
        const std::size_t hend = buf_.find("\r\n\r\n") + 4;
        head = buf_.substr(0, hend);
        status = std::atoi(head.c_str() + 9);
        buf_.erase(0, hend);
        if (ilower(head).find("transfer-encoding: chunked") == std::string::npos)
            return false;                       // caller expected chunked
        for (;;) {
            const std::size_t nl = buf_.find("\r\n");
            if (nl == std::string::npos) {
                if (!pump())
                    return false;
                continue;
            }
            const std::size_t n = std::strtoul(buf_.substr(0, nl).c_str(), nullptr, 16);
            if (buf_.size() < nl + 2 + n + 2) {
                if (!pump())
                    return false;
                continue;
            }
            if (n == 0) {
                buf_.erase(0, nl + 4);          // terminal chunk plus its CRLF
                return true;
            }
            body.append(buf_, nl + 2, n);
            buf_.erase(0, nl + 2 + n + 2);
        }
    }

    bool pump()
    {
        char tmp[65536];
        const ssize_t n = ::recv(fd_, tmp, sizeof tmp, 0);
        if (n <= 0)
            return false;
        buf_.append(tmp, std::size_t(n));
        return true;
    }

    bool read_until_eof(std::string& out)
    {
        out = buf_;
        buf_.clear();
        char tmp[8192];
        for (;;) {
            const ssize_t n = ::recv(fd_, tmp, sizeof tmp, 0);
            if (n < 0)
                return false;                   // timed out: the peer never closed
            if (n == 0)
                return true;
            out.append(tmp, std::size_t(n));
        }
    }

private:
    static std::string ilower(const std::string& s)
    {
        std::string o = s;
        for (auto& c : o)
            if (c >= 'A' && c <= 'Z')
                c = char(c - 'A' + 'a');
        return o;
    }

    int fd_ = -1;
    std::string buf_;
};

// A server on its own thread, torn down cleanly
class running_server {
public:
    explicit running_server(server_config cfg = server_config{}) : srv(std::move(cfg)) {}

    void start()
    {
        port = srv.listen("127.0.0.1", 0);
        assert(port != 0);
        th = std::thread([this] { srv.run(); });
        spin_until([this] { return srv.running(); });
    }

    ~running_server()
    {
        srv.stop();
        if (th.joinable())
            th.join();
    }

    server srv;
    std::uint16_t port = 0;
    std::thread th;
};

std::string get_line(std::uint16_t port, const std::string& target)
{
    client c(port);
    c.send_raw("GET " + target + " HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    int status = 0;
    std::string head, body;
    assert(c.read_response(status, head, body));
    return std::to_string(status) + " " + body;
}

void test_server_routing()
{
    running_server s;
    s.srv.get("/hello", [](const request&, responder r) {
        r.send(200, "text/plain", "hi");
    });
    s.srv.get("/users/:id/posts/:post", [](const request& req, responder r) {
        r.send(200, "text/plain", req.param("id") + "/" + req.param("post"));
    });
    s.srv.get("/files/*", [](const request& req, responder r) {
        r.send(200, "text/plain", req.param("*"));
    });
    s.srv.post("/hello", [](const request& req, responder r) {
        r.send(200, "text/plain", "posted:" + req.body);
    });
    s.srv.get("/q", [](const request& req, responder r) {
        r.send(200, "text/plain", req.query_param("name"));
    });
    s.start();

    assert(get_line(s.port, "/hello") == "200 hi");
    assert(get_line(s.port, "/users/42/posts/7") == "200 42/7");
    assert(get_line(s.port, "/files/a/b/c.txt") == "200 /a/b/c.txt");
    assert(get_line(s.port, "/q?name=saxon+n") == "200 saxon n");
    assert(get_line(s.port, "/nope").substr(0, 3) == "404");

    {   // wrong method on a known path is 405, with Allow
        client c(s.port);
        c.send_raw("DELETE /hello HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
        int status = 0;
        std::string head, body;
        assert(c.read_response(status, head, body));
        assert(status == 405);
        assert(head.find("Allow: GET, POST") != std::string::npos);
    }
    {   // POST with a body
        client c(s.port);
        c.send_raw("POST /hello HTTP/1.1\r\nHost: t\r\nContent-Length: 5\r\nConnection: close\r\n\r\nworld");
        int status = 0;
        std::string head, body;
        assert(c.read_response(status, head, body));
        assert(status == 200 && body == "posted:world");
    }

    pass("http server: routing, params, wildcards, 404/405");
}

void test_server_keep_alive_and_pipelining()
{
    running_server s;
    s.srv.get("/n/:i", [](const request& req, responder r) {
        r.send(200, "text/plain", req.param("i"));
    });
    s.srv.head("/n/:i", [](const request&, responder r) {
        response res(200);
        res.content("this body must not be sent");
        r.send(std::move(res));
    });
    s.start();

    {   // three requests down one connection
        client c(s.port);
        for (int i = 0; i < 3; ++i) {
            c.send_raw("GET /n/" + std::to_string(i) + " HTTP/1.1\r\nHost: t\r\n\r\n");
            int status = 0;
            std::string head, body;
            assert(c.read_response(status, head, body));
            assert(status == 200 && body == std::to_string(i));
            assert(head.find("keep-alive") != std::string::npos);
        }
    }
    {   // pipelined: three at once, answered in order
        client c(s.port);
        std::string all;
        for (int i = 0; i < 3; ++i)
            all += "GET /n/" + std::to_string(i) + " HTTP/1.1\r\nHost: t\r\n\r\n";
        c.send_raw(all);
        for (int i = 0; i < 3; ++i) {
            int status = 0;
            std::string head, body;
            assert(c.read_response(status, head, body));
            assert(status == 200);
            assert(body == std::to_string(i));      // order preserved
        }
    }
    {   // HEAD: the headers of a GET, none of the body
        client c(s.port);
        c.send_raw("HEAD /n/9 HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
        int status = 0;
        std::string head, body;
        assert(c.read_response(status, head, body, /*no_body*/ true));
        assert(status == 200);
        assert(body.empty());
        assert(head.find("Content-Length: 26") != std::string::npos);
    }
    {   // Connection: close is honoured - the server hangs up
        client c(s.port);
        c.send_raw("GET /n/1 HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
        std::string all;
        assert(c.read_until_eof(all));
        assert(all.find("Connection: close") != std::string::npos);
    }

    pass("http server: keep-alive, pipelining, HEAD, close");
}

void test_server_bodies()
{
    running_server s;
    s.srv.post("/echo", [](const request& req, responder r) {
        r.send(200, "application/octet-stream", req.body);
    });
    s.start();

    {   // chunked request body
        client c(s.port);
        c.send_raw("POST /echo HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n"
                   "Connection: close\r\n\r\n"
                   "4\r\nabcd\r\n3;x=1\r\nefg\r\n0\r\n\r\n");
        int status = 0;
        std::string head, body;
        assert(c.read_response(status, head, body));
        assert(status == 200 && body == "abcdefg");
    }
    {   // Expect: 100-continue - the interim response comes first
        client c(s.port);
        c.send_raw("POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 4\r\n"
                   "Expect: 100-continue\r\nConnection: close\r\n\r\n");
        int status = 0;
        std::string head, body;
        assert(c.read_response(status, head, body));
        assert(status == 100);
        c.send_raw("body");
        assert(c.read_response(status, head, body));
        assert(status == 200 && body == "body");
    }
    {   // a response far larger than any socket buffer: partial writes, the
        // writable interest, and reassembly all get exercised
        running_server big;
        const std::string payload(4u * 1024 * 1024, 'z');
        big.srv.get("/big", [&payload](const request&, responder r) {
            r.send(200, "application/octet-stream", payload);
        });
        big.start();

        client c(big.port);
        c.send_raw("GET /big HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
        int status = 0;
        std::string head, body;
        assert(c.read_response(status, head, body));
        assert(status == 200);
        assert(body.size() == payload.size());
        assert(body == payload);
    }
    {   // a smuggling attempt is refused at the door
        client c(s.port);
        c.send_raw("POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 5\r\n"
                   "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
        int status = 0;
        std::string head, body;
        assert(c.read_response(status, head, body));
        assert(status == 400);
        assert(head.find("Connection: close") != std::string::npos);
    }

    pass("http server: chunked, 100-continue, large responses, bad framing");
}

void test_server_async_responders()
{
    running_server s;
    std::vector<std::thread> workers;
    moveable_mutex<> worker_lock;

    // The headline: the handler returns immediately and the response is
    // completed later, from a thread that is not the loop thread
    s.srv.get("/slow", [&](const request&, responder r) {
        std::lock_guard<moveable_mutex<>> g(worker_lock);
        workers.emplace_back([r = std::move(r)]() mutable {
            std::this_thread::sleep_for(30ms);
            r.send(200, "text/plain", "late but correct");
        });
    });
    // A handler that drops its responder is a bug - and says so with a 500
    s.srv.get("/dropped", [](const request&, responder) {});
    s.start();

    {   // four concurrent slow requests, all answered
        std::vector<std::thread> clients;
        std::atomic<int> ok{0};
        for (int i = 0; i < 4; ++i)
            clients.emplace_back([&] {
                client c(s.port);
                c.send_raw("GET /slow HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
                int status = 0;
                std::string head, body;
                if (c.read_response(status, head, body) && status == 200 &&
                    body == "late but correct")
                    ok.fetch_add(1);
            });
        for (auto& t : clients)
            t.join();
        assert(ok.load() == 4);
    }

    assert(get_line(s.port, "/dropped").substr(0, 3) == "500");

    {
        std::lock_guard<moveable_mutex<>> g(worker_lock);
        for (auto& t : workers)
            t.join();
    }

    pass("http server: async responders completed off the loop thread");
}

void test_server_taps_and_moveability()
{
    // Signals as the logging and metrics surface
    std::atomic<int> opens{0}, closes{0};
    std::vector<std::string> log;
    moveable_mutex<> log_lock;

    {
        // A fully configured server, built in a factory and moved into place -
        // Asio's io_context cannot do this, and neither can httplib's server
        auto make = [&] {
            server srv;
            srv.get("/ok", [](const request&, responder r) { r.send(200, "text/plain", "ok"); });
            return srv;
        };
        std::vector<server> shelf;
        shelf.push_back(make());
        server srv = std::move(shelf.back());
        shelf.clear();

        srv.on_open().connect([&](const connection_info&) { opens.fetch_add(1); });
        srv.on_close().connect([&](const connection_info&) { closes.fetch_add(1); });
        srv.on_access().connect([&](const access_entry& e) {
            std::lock_guard<moveable_mutex<>> g(log_lock);
            log.push_back(e.method + " " + e.path + " -> " + std::to_string(e.status) +
                          " (" + std::to_string(e.response_bytes) + "B)");
        });

        const std::uint16_t port = srv.listen("127.0.0.1", 0);
        std::thread th([&] { srv.run(); });
        spin_until([&] { return srv.running(); });

        assert(get_line(port, "/ok") == "200 ok");
        assert(get_line(port, "/missing").substr(0, 3) == "404");
        spin_until([&] { return closes.load() >= 2; });

        assert(srv.total_requests() == 2);
        assert(srv.total_connections() == 2);

        srv.stop();
        th.join();
    }

    assert(opens.load() == 2);
    assert(closes.load() == 2);
    {
        std::lock_guard<moveable_mutex<>> g(log_lock);
        assert(log.size() == 2);
        assert(log[0].find("GET /ok -> 200") == 0);
        assert(log[1].find("GET /missing -> 404") == 0);
    }

    pass("http server: signal taps, and the server itself moves");
}

void test_server_streaming()
{
    running_server s;
    // A body written in pieces, length unknown when the headers go out
    s.srv.get("/stream", [](const request&, responder r) {
        auto out = r.stream(response(200).type("text/plain"));
        for (int i = 0; i < 5; ++i)
            out.write("piece" + std::to_string(i) + ";");
        out.end();
    });
    // Dropping the handle without end() must still terminate the body
    s.srv.get("/dropped", [](const request&, responder r) {
        auto out = r.stream(response(200).type("text/plain"));
        out.write("partial");
    });
    // A caller who knows the length gets it framed that way instead
    s.srv.get("/sized", [](const request&, responder r) {
        response h(200);
        h.set("Content-Length", "9");
        auto out = r.stream(std::move(h));
        out.write("abc");
        out.write("def");
        out.write("ghi");
        out.end();
    });
    // Large enough that the client cannot possibly have it all buffered
    s.srv.get("/big", [](const request&, responder r) {
        auto out = r.stream(response(200).type("application/octet-stream"));
        for (int i = 0; i < 512; ++i)
            out.write(std::string(4096, 'z'));
        out.end();
    });
    s.start();

    {   // chunked framing, reassembled by the client
        client c(s.port);
        c.send_raw("GET /stream HTTP/1.1\r\nHost: t\r\n\r\n");
        int status = 0;
        std::string head, body;
        assert(c.read_chunked(status, head, body));
        assert(status == 200);
        assert(head.find("Transfer-Encoding: chunked") != std::string::npos);
        assert(head.find("Content-Length") == std::string::npos);
        assert(body == "piece0;piece1;piece2;piece3;piece4;");
    }
    {   // a dropped stream still terminates rather than hanging the client
        client c(s.port);
        c.send_raw("GET /dropped HTTP/1.1\r\nHost: t\r\n\r\n");
        int status = 0;
        std::string head, body;
        assert(c.read_chunked(status, head, body));
        assert(status == 200 && body == "partial");
    }
    {   // a known length is framed as Content-Length, not chunked
        client c(s.port);
        c.send_raw("GET /sized HTTP/1.1\r\nHost: t\r\n\r\n");
        int status = 0;
        std::string head, body;
        assert(c.read_response(status, head, body));
        assert(status == 200);
        assert(head.find("Transfer-Encoding") == std::string::npos);
        assert(body == "abcdefghi");
    }
    {   // 2 MiB streamed in 4 KiB pieces, arriving intact and in order
        client c(s.port);
        c.send_raw("GET /big HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
        int status = 0;
        std::string head, body;
        assert(c.read_chunked(status, head, body));
        assert(status == 200);
        assert(body.size() == 512u * 4096);
        assert(body.find_first_not_of('z') == std::string::npos);
    }
    {   // keep-alive survives a streamed response: the next request is served
        client c(s.port);
        c.send_raw("GET /stream HTTP/1.1\r\nHost: t\r\n\r\n");
        int status = 0;
        std::string head, body;
        assert(c.read_chunked(status, head, body));
        assert(body == "piece0;piece1;piece2;piece3;piece4;");
        c.send_raw("GET /sized HTTP/1.1\r\nHost: t\r\n\r\n");
        assert(c.read_response(status, head, body));
        assert(status == 200 && body == "abcdefghi");
    }

    pass("http server: streamed responses (chunked, sized, dropped, keep-alive)");
}

void test_server_timeouts()
{
    server_config cfg;
    cfg.request_timeout = std::chrono::seconds{1};
    cfg.idle_timeout = std::chrono::seconds{2};
    cfg.sweep_interval = std::chrono::milliseconds{50};

    running_server s(cfg);
    s.srv.get("/x", [](const request&, responder r) { r.send(200, "text/plain", "x"); });
    s.start();

    // A slowloris: headers begun and never finished. The sweep must reap it.
    client c(s.port);
    c.send_raw("GET /x HTTP/1.1\r\nHost: t\r\nX-Partial: ");
    int status = 0;
    std::string head, body;
    assert(c.read_response(status, head, body));
    assert(status == 408);

    pass("http server: slow-request timeout closes the connection");
}

} // namespace

void run_http_server_tests()
{
    test_parser_basics();
    test_parser_fragmentation();
    test_parser_smuggling_defences();
    test_parser_transfer_encoding();
    test_parser_limits_and_versions();
    test_parser_targets();

    test_server_routing();
    test_server_keep_alive_and_pipelining();
    test_server_bodies();
    test_server_async_responders();
    test_server_taps_and_moveability();
    test_server_streaming();
    test_server_timeouts();
}

#else // !SNICHOLLS_HAS_HTTP_SERVER

// Windows: the server follows the reactor, which is POSIX-only in phase 1
void run_http_server_tests() {}

#endif
