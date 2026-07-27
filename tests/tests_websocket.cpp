//
//  tests_websocket.cpp
//  TSMoveables
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - unit tests for the WebSocket protocol delegate
//
//  Two halves again. First the primitives, against published vectors: SHA-1
//  and base64 from their RFCs, the accept-key example from RFC 6455 §1.3
//  itself, and a UTF-8 validator tortured with the overlong, surrogate and
//  out-of-range forms Autobahn checks. Then a real server on loopback driven
//  by a hand-rolled WebSocket client - because the interesting claim is that
//  an HTTP connection changes protocol mid-flight and keeps working.
//

#include "test_helpers.hpp"

#include "../TSMoveables/http/websocket.hpp"

#if SNICHOLLS_HAS_WEBSOCKET

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace snicholls;
using namespace snicholls::http;
using namespace std::chrono_literals;

namespace {

std::string hex(const unsigned char* p, std::size_t n)
{
    static const char* d = "0123456789abcdef";
    std::string o;
    for (std::size_t i = 0; i < n; ++i) {
        o.push_back(d[p[i] >> 4]);
        o.push_back(d[p[i] & 15]);
    }
    return o;
}

void test_ws_primitives()
{
    // RFC 3174 test vectors
    {
        const auto a = http::detail::sha1("abc");
        assert(hex(a.data(), a.size()) == "a9993e364706816aba3e25717850c26c9cd0d89d");
        const auto b = http::detail::sha1("");
        assert(hex(b.data(), b.size()) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
        const auto c = http::detail::sha1(
            "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
        assert(hex(c.data(), c.size()) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
        // Spans several blocks, so the length padding is exercised properly
        const auto d = http::detail::sha1(std::string(1000, 'a'));
        assert(hex(d.data(), d.size()) == "291e9a6c66994949b57ba5e650361e98fc36b1ba");
    }

    // RFC 4648 base64 vectors, including both padding shapes
    {
        auto b64 = [](const std::string& s) {
            return http::detail::base64(reinterpret_cast<const unsigned char*>(s.data()), s.size());
        };
        assert(b64("") == "");
        assert(b64("f") == "Zg==");
        assert(b64("fo") == "Zm8=");
        assert(b64("foo") == "Zm9v");
        assert(b64("foob") == "Zm9vYg==");
        assert(b64("fooba") == "Zm9vYmE=");
        assert(b64("foobar") == "Zm9vYmFy");
    }

    // The worked example from RFC 6455 §1.3 - the whole handshake in one line
    {
        const auto d = http::detail::sha1(std::string("dGhlIHNhbXBsZSBub25jZQ==") +
                                    "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
        assert(http::detail::base64(d.data(), d.size()) == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    }

    pass("websocket: SHA-1, base64 and the RFC 6455 accept key");
}

void test_ws_utf8()
{
    assert(http::detail::valid_utf8(""));
    assert(http::detail::valid_utf8("hello"));
    assert(http::detail::valid_utf8("\xc3\xa9"));                  // é
    assert(http::detail::valid_utf8("\xe2\x82\xac"));              // €
    assert(http::detail::valid_utf8("\xf0\x9f\x98\x80"));          // emoji

    assert(!http::detail::valid_utf8("\xc3"));                     // truncated
    assert(!http::detail::valid_utf8("\x80"));                     // stray continuation
    assert(!http::detail::valid_utf8("\xc0\xaf"));                 // overlong '/'
    assert(!http::detail::valid_utf8("\xe0\x80\xaf"));             // overlong, 3 bytes
    assert(!http::detail::valid_utf8("\xf0\x80\x80\xaf"));         // overlong, 4 bytes
    assert(!http::detail::valid_utf8("\xed\xa0\x80"));             // UTF-16 surrogate half
    assert(!http::detail::valid_utf8("\xf4\x90\x80\x80"));         // beyond U+10FFFF
    assert(!http::detail::valid_utf8("\xfe"));                     // not a lead byte
    assert(!http::detail::valid_utf8("\xc3\x28"));                 // bad continuation

    pass("websocket: UTF-8 validation (overlong, surrogate, out of range)");
}

// ------------------------------------------------------------- a ws client

class ws_client {
public:
    bool connect_to(std::uint16_t port, const char* path = "/ws")
    {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        assert(fd_ >= 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0)
            return false;
        const int on = 1;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
        timeval tv{};
        tv.tv_sec = 5;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

        const std::string req =
            std::string("GET ") + path + " HTTP/1.1\r\nHost: t\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n";
        send_raw(req);

        std::string head;
        while (head.find("\r\n\r\n") == std::string::npos) {
            char t[1024];
            const ssize_t n = ::recv(fd_, t, sizeof t, 0);
            if (n <= 0)
                return false;
            head.append(t, std::size_t(n));
        }
        const std::size_t end = head.find("\r\n\r\n") + 4;
        buf_.assign(head, end, std::string::npos);
        status_ = std::atoi(head.c_str() + 9);
        accept_ok_ = head.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") !=
                     std::string::npos;
        return true;
    }

    ~ws_client()
    {
        if (fd_ >= 0)
            ::close(fd_);
    }

    int status() const noexcept { return status_; }
    bool accept_ok() const noexcept { return accept_ok_; }

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

    // Client frames are masked, as RFC 6455 requires of them
    void send_frame(ws_opcode op, const std::string& payload, bool fin = true, bool mask = true)
    {
        std::string f;
        f.push_back(char((fin ? 0x80 : 0x00) | static_cast<std::uint8_t>(op)));
        const std::size_t n = payload.size();
        const char m = mask ? char(0x80) : char(0x00);
        if (n < 126) {
            f.push_back(char(m | char(n)));
        } else if (n <= 0xFFFF) {
            f.push_back(char(m | char(126)));
            f.push_back(char((n >> 8) & 0xff));
            f.push_back(char(n & 0xff));
        } else {
            f.push_back(char(m | char(127)));
            for (int i = 7; i >= 0; --i)
                f.push_back(char((std::uint64_t(n) >> (i * 8)) & 0xff));
        }
        if (mask) {
            const unsigned char key[4] = {0x37, 0xfa, 0x21, 0x3d};
            for (int i = 0; i < 4; ++i)
                f.push_back(char(key[i]));
            for (std::size_t i = 0; i < n; ++i)
                f.push_back(char(static_cast<unsigned char>(payload[i]) ^ key[i & 3]));
        } else {
            f += payload;
        }
        send_raw(f);
    }

    // Read one server frame. Server frames are never masked.
    bool read_frame(ws_opcode& op, std::string& payload, bool& fin)
    {
        while (!parse(op, payload, fin)) {
            char t[65536];
            const ssize_t n = ::recv(fd_, t, sizeof t, 0);
            if (n <= 0)
                return false;
            buf_.append(t, std::size_t(n));
        }
        return true;
    }

private:
    bool parse(ws_opcode& op, std::string& payload, bool& fin)
    {
        if (buf_.size() < 2)
            return false;
        const unsigned char* p = reinterpret_cast<const unsigned char*>(buf_.data());
        fin = (p[0] & 0x80) != 0;
        op = static_cast<ws_opcode>(p[0] & 0x0f);
        assert((p[1] & 0x80) == 0 && "server frames must not be masked");
        std::uint64_t len = p[1] & 0x7f;
        std::size_t hdr = 2;
        if (len == 126) {
            if (buf_.size() < 4) return false;
            len = (std::uint64_t(p[2]) << 8) | p[3];
            hdr = 4;
        } else if (len == 127) {
            if (buf_.size() < 10) return false;
            len = 0;
            for (int i = 0; i < 8; ++i)
                len = (len << 8) | p[2 + i];
            hdr = 10;
        }
        if (buf_.size() < hdr + len)
            return false;
        payload.assign(buf_, hdr, std::size_t(len));
        buf_.erase(0, hdr + std::size_t(len));
        return true;
    }

    int fd_ = -1;
    int status_ = 0;
    bool accept_ok_ = false;
    std::string buf_;
};

class ws_server {
public:
    ws_server()
    {
        srv.get("/ws", websocket_route([this](websocket ws) {
            // The tracked form: the socket is an argument, not a capture
            ws.on_message([this](websocket sock, const ws_message& m) {
                ++messages;
                if (m.is_text)
                    sock.send_text("echo:" + m.data);
                else
                    sock.send_binary(m.data);
            });
            ws.on_close([this](websocket, std::uint16_t code, const std::string&) {
                last_close.store(code);
                ++closes;
            });
            sockets.push_back(ws.share());
            opens.fetch_add(1, std::memory_order_release);
        }));
        srv.get("/plain", [](const request&, responder r) {
            r.send(200, "text/plain", "not a websocket");
        });
        port = srv.listen("127.0.0.1", 0);
        th = std::thread([this] { srv.run(); });
        spin_until([this] { return srv.running(); });
    }

    ~ws_server()
    {
        srv.stop();
        if (th.joinable())
            th.join();
    }

    server srv;
    std::uint16_t port = 0;
    std::thread th;
    std::atomic<int> messages{0};
    std::atomic<int> opens{0};
    std::atomic<int> closes{0};
    std::atomic<std::uint16_t> last_close{0};
    std::vector<websocket> sockets;      // loop thread only
};

void test_ws_handshake_and_echo()
{
    ws_server s;
    ws_client c;
    assert(c.connect_to(s.port));
    assert(c.status() == 101);
    assert(c.accept_ok());              // the RFC's own key/accept pair

    c.send_frame(ws_opcode::text, "hello");
    ws_opcode op;
    std::string payload;
    bool fin = false;
    assert(c.read_frame(op, payload, fin));
    assert(op == ws_opcode::text);
    assert(fin);
    assert(payload == "echo:hello");

    // Binary round trip, and a payload needing the 16-bit length form
    const std::string big(5000, 'b');
    c.send_frame(ws_opcode::binary, big);
    assert(c.read_frame(op, payload, fin));
    assert(op == ws_opcode::binary);
    assert(payload == big);

    // And the 64-bit length form
    const std::string huge(70000, 'h');
    c.send_frame(ws_opcode::binary, huge);
    assert(c.read_frame(op, payload, fin));
    assert(payload.size() == huge.size());

    pass("websocket: upgrade handshake, echo, extended lengths");
}

void test_ws_fragmentation_and_control()
{
    ws_server s;
    ws_client c;
    assert(c.connect_to(s.port));

    // A message split across three frames reassembles before delivery
    c.send_frame(ws_opcode::text, "one ", /*fin*/ false);
    c.send_frame(ws_opcode::continuation, "two ", false);
    c.send_frame(ws_opcode::continuation, "three", true);
    ws_opcode op;
    std::string payload;
    bool fin = false;
    assert(c.read_frame(op, payload, fin));
    assert(payload == "echo:one two three");

    // Ping is answered with a pong carrying the same payload
    c.send_frame(ws_opcode::ping, "ping-body");
    assert(c.read_frame(op, payload, fin));
    assert(op == ws_opcode::pong);
    assert(payload == "ping-body");

    // Close is a two-way handshake, and the code comes back
    c.send_frame(ws_opcode::close, std::string("\x03\xe8") + "bye");   // 1000
    assert(c.read_frame(op, payload, fin));
    assert(op == ws_opcode::close);
    assert(payload.size() >= 2);
    const std::uint16_t code =
        std::uint16_t((static_cast<unsigned char>(payload[0]) << 8) |
                      static_cast<unsigned char>(payload[1]));
    assert(code == 1000);
    spin_until([&] { return s.closes.load() > 0; });
    assert(s.last_close.load() == 1000);

    pass("websocket: fragmentation, ping/pong, close handshake");
}

void test_ws_protocol_errors()
{
    {   // An unmasked client frame is a protocol error, not a leniency
        ws_server s;
        ws_client c;
        assert(c.connect_to(s.port));
        c.send_frame(ws_opcode::text, "unmasked", true, /*mask*/ false);
        ws_opcode op;
        std::string payload;
        bool fin = false;
        assert(c.read_frame(op, payload, fin));
        assert(op == ws_opcode::close);
        const std::uint16_t code =
            std::uint16_t((static_cast<unsigned char>(payload[0]) << 8) |
                          static_cast<unsigned char>(payload[1]));
        assert(code == ws_protocol_error);
    }
    {   // Invalid UTF-8 in a text frame closes with 1007
        ws_server s;
        ws_client c;
        assert(c.connect_to(s.port));
        c.send_frame(ws_opcode::text, "\xc0\xaf");          // overlong
        ws_opcode op;
        std::string payload;
        bool fin = false;
        assert(c.read_frame(op, payload, fin));
        assert(op == ws_opcode::close);
        const std::uint16_t code =
            std::uint16_t((static_cast<unsigned char>(payload[0]) << 8) |
                          static_cast<unsigned char>(payload[1]));
        assert(code == ws_invalid_payload);
    }
    {   // A handshake without the WebSocket headers is just a bad request
        ws_server s;
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(s.port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        assert(::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
        const std::string req = "GET /ws HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n";
        [[maybe_unused]] ssize_t w = ::send(fd, req.data(), req.size(), 0);
        char buf[512] = {0};
        const ssize_t n = ::recv(fd, buf, sizeof buf - 1, 0);
        assert(n > 0);
        assert(std::atoi(buf + 9) == 400);
        ::close(fd);
    }

    pass("websocket: unmasked frames, bad UTF-8 and bad handshakes rejected");
}

void test_ws_coexists_with_http()
{
    // The point of a protocol delegate: the same server, port and loop still
    // serve ordinary HTTP while WebSockets are live on other connections
    ws_server s;
    ws_client c;
    assert(c.connect_to(s.port));
    assert(c.status() == 101);

    {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(s.port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        assert(::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
        const std::string req = "GET /plain HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n";
        [[maybe_unused]] ssize_t w = ::send(fd, req.data(), req.size(), 0);
        std::string got;
        char buf[1024];
        for (;;) {
            const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
            if (n <= 0)
                break;
            got.append(buf, std::size_t(n));
        }
        ::close(fd);
        assert(got.find("200 OK") != std::string::npos);
        assert(got.find("not a websocket") != std::string::npos);
    }

    // ...and the upgraded connection is still alive afterwards
    c.send_frame(ws_opcode::text, "still here");
    ws_opcode op;
    std::string payload;
    bool fin = false;
    assert(c.read_frame(op, payload, fin));
    assert(payload == "echo:still here");

    pass("websocket: HTTP and WebSocket share one server and one loop");
}

void test_ws_server_initiated()
{
    // Sending from outside the read loop - the server pushing to a client,
    // which is the whole reason a WebSocket exists
    ws_server s;
    ws_client c;
    assert(c.connect_to(s.port));
    // Wait for the upgrade to be registered on the loop thread before pushing.
    // An earlier version posted a task capturing a local that had already gone
    // out of scope by the time the loop ran it - a dangling reference and a
    // data race, which is precisely what ThreadSanitizer is for.
    spin_until([&] { return s.opens.load(std::memory_order_acquire) > 0; });

    std::atomic<bool> sent{false};
    s.srv.loop().post([&] {
        for (auto& w : s.sockets)
            w.send_text("push");
        sent.store(true);
    });
    spin_until([&] { return sent.load(); });

    ws_opcode op;
    std::string payload;
    bool fin = false;
    assert(c.read_frame(op, payload, fin));
    assert(op == ws_opcode::text);
    assert(payload == "push");

    pass("websocket: server-initiated push");
}

} // namespace

void run_websocket_tests()
{
    test_ws_primitives();
    test_ws_utf8();
    test_ws_handshake_and_echo();
    test_ws_fragmentation_and_control();
    test_ws_protocol_errors();
    test_ws_coexists_with_http();
    test_ws_server_initiated();
}

#else // !SNICHOLLS_HAS_WEBSOCKET

void run_websocket_tests() {}

#endif
