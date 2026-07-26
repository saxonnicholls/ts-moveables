//
//  tests_tls.cpp
//  TSMoveables
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - tests for the OpenSSL transport delegate
//
//  A separate program in a separate directory on purpose. The library core is
//  dependency-free and the main suite must stay that way, so this file is kept
//  outside the tests/*.cpp glob that builds it: `make test-tls` builds this one
//  binary, and it is the only thing in the repo that links OpenSSL.
//
//  The claim under test is the two-axis split. Swapping the transport should
//  turn http into https and ws into wss with no change to either protocol
//  delegate - so the last test here runs a WebSocket over TLS, which is the
//  whole architecture in one exchange: an encrypted connection that starts as
//  HTTP, upgrades protocol mid-flight, and keeps going.
//
//  The certificate is generated in memory at start-up, so there is nothing to
//  check in, nothing to expire, and no key on disk.
//

#include "../test_helpers.hpp"

#include "../../TSMoveables/tls_openssl.hpp"
#include "../../TSMoveables/websocket.hpp"

#if SNICHOLLS_HAS_TLS

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509v3.h>

using namespace snicholls;
using namespace snicholls::http;
using namespace std::chrono_literals;

namespace {

// ------------------------------------------------ a throwaway certificate

struct self_signed {
    EVP_PKEY* key = nullptr;
    X509* cert = nullptr;

    self_signed()
    {
        key = EVP_RSA_gen(2048);
        assert(key && "key generation");

        cert = X509_new();
        assert(cert);
        X509_set_version(cert, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
        X509_gmtime_adj(X509_getm_notBefore(cert), 0);
        X509_gmtime_adj(X509_getm_notAfter(cert), 60L * 60 * 24);
        X509_set_pubkey(cert, key);

        X509_NAME* name = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
        X509_set_issuer_name(cert, name);       // self-signed: issuer is subject
        assert(X509_sign(cert, key, EVP_sha256()) > 0);
    }

    ~self_signed()
    {
        if (cert) X509_free(cert);
        if (key) EVP_PKEY_free(key);
    }
};

// ------------------------------------------------------------- TLS client

class tls_client {
public:
    bool connect_to(std::uint16_t port, const char* alpn = nullptr)
    {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0)
            return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0)
            return false;
        const int on = 1;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
        timeval tv{};
        tv.tv_sec = 10;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

        ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ctx_)
            return false;
        ssl_ = SSL_new(ctx_);
        if (!ssl_)
            return false;
        if (alpn) {
            std::string w;
            w.push_back(char(std::strlen(alpn)));
            w += alpn;
            SSL_set_alpn_protos(ssl_, reinterpret_cast<const unsigned char*>(w.data()),
                                unsigned(w.size()));
        }
        SSL_set_fd(ssl_, fd_);
        return SSL_connect(ssl_) == 1;          // self-signed: we do not verify here
    }

    ~tls_client()
    {
        if (ssl_) { SSL_shutdown(ssl_); SSL_free(ssl_); }
        if (ctx_) SSL_CTX_free(ctx_);
        if (fd_ >= 0) ::close(fd_);
    }

    std::string negotiated_alpn() const
    {
        const unsigned char* p = nullptr;
        unsigned len = 0;
        SSL_get0_alpn_selected(ssl_, &p, &len);
        return (p && len) ? std::string(reinterpret_cast<const char*>(p), len) : std::string();
    }

    bool write_all(const std::string& s)
    {
        std::size_t off = 0;
        while (off < s.size()) {
            const int n = SSL_write(ssl_, s.data() + off, int(s.size() - off));
            if (n <= 0)
                return false;
            off += std::size_t(n);
        }
        return true;
    }

    bool read_http(int& status, std::string& head, std::string& body, bool no_body = false)
    {
        for (;;) {
            const std::size_t hend = buf_.find("\r\n\r\n");
            if (hend != std::string::npos) {
                head = buf_.substr(0, hend + 4);
                status = std::atoi(head.c_str() + 9);
                std::size_t want = 0;
                const std::size_t cl = lower(head).find("\r\ncontent-length:");
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
            if (!pump())
                return false;
        }
    }

    // Minimal WebSocket framing over the TLS stream
    void send_ws(ws_opcode op, const std::string& payload)
    {
        std::string f;
        f.push_back(char(0x80 | static_cast<std::uint8_t>(op)));
        const std::size_t n = payload.size();
        assert(n < 126);
        f.push_back(char(0x80 | char(n)));      // client frames are masked
        const unsigned char key[4] = {0x11, 0x22, 0x33, 0x44};
        for (int i = 0; i < 4; ++i)
            f.push_back(char(key[i]));
        for (std::size_t i = 0; i < n; ++i)
            f.push_back(char(static_cast<unsigned char>(payload[i]) ^ key[i & 3]));
        write_all(f);
    }

    bool read_ws(ws_opcode& op, std::string& payload)
    {
        for (;;) {
            if (buf_.size() >= 2) {
                const unsigned char* p = reinterpret_cast<const unsigned char*>(buf_.data());
                op = static_cast<ws_opcode>(p[0] & 0x0f);
                assert((p[1] & 0x80) == 0 && "server frames must not be masked");
                std::size_t len = p[1] & 0x7f, hdr = 2;
                if (len == 126 && buf_.size() >= 4) {
                    len = (std::size_t(p[2]) << 8) | p[3];
                    hdr = 4;
                }
                if (buf_.size() >= hdr + len) {
                    payload.assign(buf_, hdr, len);
                    buf_.erase(0, hdr + len);
                    return true;
                }
            }
            if (!pump())
                return false;
        }
    }

    void consume_handshake_head()
    {
        while (buf_.find("\r\n\r\n") == std::string::npos)
            if (!pump())
                return;
        buf_.erase(0, buf_.find("\r\n\r\n") + 4);
    }

private:
    bool pump()
    {
        char t[16384];
        const int n = SSL_read(ssl_, t, int(sizeof t));
        if (n <= 0)
            return false;
        buf_.append(t, std::size_t(n));
        return true;
    }

    static std::string lower(std::string s)
    {
        for (auto& c : s)
            if (c >= 'A' && c <= 'Z')
                c = char(c - 'A' + 'a');
        return s;
    }

    int fd_ = -1;
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    std::string buf_;
};

// --------------------------------------------------------- an HTTPS server

class https_server {
public:
    explicit https_server(const self_signed& cred)
    {
        tls.use_certificate_and_key(cred.cert, cred.key);
        tls.set_alpn({"http/1.1"});
        srv.transport_factory(tls.factory());

        srv.get("/hello", [](const request&, responder r) {
            r.send(200, "text/plain", "secure hello");
        });
        srv.post("/echo", [](const request& req, responder r) {
            r.send(200, "application/octet-stream", req.body);
        });
        srv.get("/ws", websocket_route([](websocket ws) {
            auto c = ws.on_message().connect([ws = ws.share()](const ws_message& m) {
                ws.send_text("secure-echo:" + m.data);
            });
            ws.keep(std::move(c));
        }));

        port = srv.listen("127.0.0.1", 0);
        th = std::thread([this] { srv.run(); });
        spin_until([this] { return srv.running(); });
    }

    ~https_server()
    {
        srv.stop();
        if (th.joinable())
            th.join();
    }

    openssl_context tls;
    server srv;
    std::uint16_t port = 0;
    std::thread th;
};

void test_tls_request_response(const self_signed& cred)
{
    https_server s(cred);
    tls_client c;
    assert(c.connect_to(s.port));

    assert(c.write_all("GET /hello HTTP/1.1\r\nHost: t\r\n\r\n"));
    int status = 0;
    std::string head, body;
    assert(c.read_http(status, head, body));
    assert(status == 200);
    assert(body == "secure hello");

    // Keep-alive across the same TLS session
    for (int i = 0; i < 3; ++i) {
        assert(c.write_all("GET /hello HTTP/1.1\r\nHost: t\r\n\r\n"));
        assert(c.read_http(status, head, body));
        assert(status == 200 && body == "secure hello");
    }

    pass("tls: HTTPS request/response and keep-alive over one session");
}

void test_tls_alpn(const self_signed& cred)
{
    https_server s(cred);
    tls_client c;
    assert(c.connect_to(s.port, "http/1.1"));
    assert(c.negotiated_alpn() == "http/1.1");      // the h2/h3 selector, already wired

    pass("tls: ALPN negotiated and reported");
}

void test_tls_large_body(const self_signed& cred)
{
    // Bigger than one TLS record, so record splitting and reassembly are real
    https_server s(cred);
    tls_client c;
    assert(c.connect_to(s.port));

    const std::string payload(512u * 1024, 'x');
    std::string req = "POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: " +
                      std::to_string(payload.size()) + "\r\n\r\n";
    req += payload;
    assert(c.write_all(req));

    int status = 0;
    std::string head, body;
    assert(c.read_http(status, head, body));
    assert(status == 200);
    assert(body.size() == payload.size());
    assert(body == payload);

    pass("tls: 512 KiB round trip across many TLS records");
}

void test_wss(const self_signed& cred)
{
    // The architecture in one exchange: TLS transport underneath, HTTP that
    // upgrades to WebSocket on top, and neither delegate knowing about the
    // other. `wss` needed no code of its own.
    https_server s(cred);
    tls_client c;
    assert(c.connect_to(s.port));

    assert(c.write_all(
        "GET /ws HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n"));
    c.consume_handshake_head();

    c.send_ws(ws_opcode::text, "over tls");
    ws_opcode op;
    std::string payload;
    assert(c.read_ws(op, payload));
    assert(op == ws_opcode::text);
    assert(payload == "secure-echo:over tls");

    pass("tls: wss - WebSocket over TLS, with no code joining the two");
}

} // namespace

int main()
{
    self_signed cred;
    test_tls_request_response(cred);
    test_tls_alpn(cred);
    test_tls_large_body(cred);
    test_wss(cred);

    std::printf("\nAll %d TLS tests passed\n", tests_run);
    return 0;
}

#else // !SNICHOLLS_HAS_TLS

int main()
{
    std::printf("TLS tests: POSIX-only server - skipped\n");
    return 0;
}

#endif
