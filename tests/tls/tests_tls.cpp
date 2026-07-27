//
//  tests_tls.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - tests for the TLS transport delegates
//
//  A separate program in a separate directory on purpose. The library core is
//  dependency-free and the main suite must stay that way, so this file is kept
//  outside the tests/*.cpp glob that builds it: `make test-tls` builds this one
//  binary, and it is the only thing in the repo that links a TLS library.
//
//  The claim under test is the two-axis split. Swapping the transport should
//  turn http into https and ws into wss with no change to either protocol
//  delegate - so the last test here runs a WebSocket over TLS, which is the
//  whole architecture in one exchange: an encrypted connection that starts as
//  HTTP, upgrades protocol mid-flight, and keeps going.
//
//  Every test body is a template over the backend and each one runs twice, once
//  against OpenSSL and once against mbedTLS. That is not thoroughness for its
//  own sake: the two libraries are driven in opposite directions - OpenSSL
//  moves bytes through memory BIOs we hand it, mbedTLS calls callbacks we
//  supply - so a test body that does not have to change between them is the
//  evidence that `transport_delegate` describes TLS rather than describing
//  OpenSSL. Duplicating the bodies would have destroyed exactly that evidence.
//
//  The certificate is generated in memory at start-up, so there is nothing to
//  check in, nothing to expire, and no key on disk. It is generated once, with
//  OpenSSL, and handed to both backends - to OpenSSL as X509*/EVP_PKEY* and to
//  mbedTLS as the same pair serialised to PEM. mbedTLS can write certificates
//  too, but a second generator would mean the two servers were not proving
//  themselves on identical credentials, and the binary already links OpenSSL
//  for the client either way.
//
//  The client is OpenSSL in both runs, on purpose: it makes the mbedTLS server
//  answer to an independent implementation rather than to its own idea of the
//  protocol.
//
//  If mbedTLS is not installed the build simply omits that half - see the
//  test-tls target in the Makefile.
//

#include "../test_helpers.hpp"

#include "../../TSMoveables/tls/openssl.hpp"

#if defined(SNICHOLLS_TEST_MBEDTLS)
#include "../../TSMoveables/tls/mbedtls.hpp"
#endif

#include "../../TSMoveables/http/websocket.hpp"

#if SNICHOLLS_HAS_TLS

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/bio.h>
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
    std::string cert_pem;                       // the same credential, for mbedTLS
    std::string key_pem;

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

        cert_pem = to_pem([this](BIO* b) { return PEM_write_bio_X509(b, cert) == 1; });
        key_pem = to_pem([this](BIO* b) {
            return PEM_write_bio_PrivateKey(b, key, nullptr, nullptr, 0, nullptr, nullptr) == 1;
        });
        assert(!cert_pem.empty() && !key_pem.empty());
    }

    ~self_signed()
    {
        if (cert) X509_free(cert);
        if (key) EVP_PKEY_free(key);
    }

private:
    template <typename Writer>
    static std::string to_pem(Writer write)
    {
        BIO* b = BIO_new(BIO_s_mem());
        assert(b);
        const bool ok = write(b);
        char* p = nullptr;
        const long n = BIO_get_mem_data(b, &p);
        std::string out = ok && n > 0 ? std::string(p, std::size_t(n)) : std::string();
        BIO_free(b);
        return out;
    }
};

// ------------------------------------------------------------- the backends
//
// One trait per backend, holding the two things that genuinely differ: the
// context type, and how a certificate gets into it. Nothing below this point
// mentions either library by name.

struct openssl_backend {
    using context = openssl_context;
    static const char* label() { return "openssl"; }
    static void configure(context& c, const self_signed& cred)
    {
        c.use_certificate_and_key(cred.cert, cred.key);
    }
};

#if defined(SNICHOLLS_TEST_MBEDTLS)
struct mbedtls_backend {
    using context = mbedtls_context;
    static const char* label() { return "mbedtls"; }
    static void configure(context& c, const self_signed& cred)
    {
        c.use_certificate_and_key(cred.cert_pem, cred.key_pem);
    }
};
#endif

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

// ------------------------------------------------- a client with no socket
//
// A transport delegate is supposed to be a byte transformer with a handshake,
// so it should be drivable from one thread with nothing but two std::strings -
// no port, no reactor, no descriptor. This is the counterparty for that: an
// OpenSSL client on memory BIOs, deliberately the same implementation in both
// runs so the backend under test is never asked to be its own peer.

class offline_client {
public:
    explicit offline_client(const unsigned char* alpn = nullptr, std::size_t alpn_len = 0)
    {
        ctx_ = SSL_CTX_new(TLS_client_method());
        assert(ctx_);
        ssl_ = SSL_new(ctx_);
        assert(ssl_);
        if (alpn)
            assert(SSL_set_alpn_protos(ssl_, alpn, unsigned(alpn_len)) == 0);
        BIO* rbio = BIO_new(BIO_s_mem());
        wbio_ = BIO_new(BIO_s_mem());
        BIO_set_mem_eof_return(rbio, -1);
        BIO_set_mem_eof_return(wbio_, -1);
        rbio_ = rbio;
        SSL_set_bio(ssl_, rbio_, wbio_);        // SSL owns both from here
        SSL_set_connect_state(ssl_);
    }

    ~offline_client()
    {
        if (ssl_) SSL_free(ssl_);
        if (ctx_) SSL_CTX_free(ctx_);
    }

    offline_client(const offline_client&) = delete;
    offline_client& operator=(const offline_client&) = delete;

    // Shuttle bytes back and forth until both ends say they are done. Returns
    // what the transport returned, so a fatal handshake reads as false rather
    // than as a stall.
    bool run(transport_delegate& server, std::string& app_in, int rounds = 24)
    {
        std::string to_server, to_client;
        for (int i = 0; i < rounds; ++i) {
            SSL_do_handshake(ssl_);
            char buf[16384];
            for (;;) {
                const int r = BIO_read(wbio_, buf, int(sizeof buf));
                if (r <= 0)
                    break;
                to_server.append(buf, std::size_t(r));
            }
            to_client.clear();
            if (!server.wire_in(to_server.data(), to_server.size(), app_in, to_client))
                return false;
            to_server.clear();
            if (!to_client.empty())
                assert(BIO_write(rbio_, to_client.data(), int(to_client.size())) > 0);
            if (server.established() && finished())
                break;
        }
        return true;
    }

    bool finished() const { return SSL_is_init_finished(ssl_) != 0; }
    SSL* native() noexcept { return ssl_; }

private:
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    BIO* rbio_ = nullptr;                       // ciphertext heading to the client
    BIO* wbio_ = nullptr;                       // ciphertext the client produced
};

// --------------------------------------------------------- an HTTPS server
//
// Identical for both backends apart from the two lines the trait supplies -
// which is the whole point of the exercise

template <class Backend>
class https_server {
public:
    explicit https_server(const self_signed& cred)
    {
        Backend::configure(tls, cred);
        tls.set_alpn({"http/1.1"});
        srv.transport_factory(tls.factory());

        srv.get("/hello", [](const request&, responder r) {
            r.send(200, "text/plain", "secure hello");
        });
        srv.post("/echo", [](const request& req, responder r) {
            r.send(200, "application/octet-stream", req.body);
        });
        srv.get("/ws", websocket_route([](websocket ws) {
            ws.on_message([](websocket sock, const ws_message& m) {
                sock.send_text("secure-echo:" + m.data);
            });
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

    typename Backend::context tls;
    server srv;
    std::uint16_t port = 0;
    std::thread th;
};

// Test names carry the backend so a failure says which one broke
std::string tagged(const char* backend, const char* what)
{
    return std::string("tls[") + backend + "]: " + what;
}

template <class Backend>
void test_tls_request_response(const self_signed& cred)
{
    https_server<Backend> s(cred);
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

    pass(tagged(Backend::label(),
                "HTTPS request/response and keep-alive over one session").c_str());
}

template <class Backend>
void test_tls_alpn(const self_signed& cred)
{
    https_server<Backend> s(cred);
    tls_client c;
    assert(c.connect_to(s.port, "http/1.1"));
    assert(c.negotiated_alpn() == "http/1.1");      // the h2/h3 selector, already wired

    pass(tagged(Backend::label(), "ALPN negotiated and reported").c_str());
}

template <class Backend>
void test_tls_large_body(const self_signed& cred)
{
    // Bigger than one TLS record, so record splitting and reassembly are real
    https_server<Backend> s(cred);
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

    pass(tagged(Backend::label(), "512 KiB round trip across many TLS records").c_str());
}

template <class Backend>
void test_wss(const self_signed& cred)
{
    // The architecture in one exchange: TLS transport underneath, HTTP that
    // upgrades to WebSocket on top, and neither delegate knowing about the
    // other. `wss` needed no code of its own.
    https_server<Backend> s(cred);
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

    pass(tagged(Backend::label(), "wss - WebSocket over TLS, with no code joining the two").c_str());
}

// Three things are checked here that the server-level tests cannot reach:
//
//   - `alpn()` on the *server* side. The ALPN test above only inspects what the
//     client was told; this is the value a future h2 selector would actually
//     read. The lists are crossed on purpose - the server prefers h2, the
//     client offers http/1.1 first - so the answer is only "h2" if server
//     preference is really being applied, on both backends.
//   - Plaintext written before the handshake finishes. Both transports have to
//     hold it back and release it on establishment, and no server test ever
//     provokes that ordering.
//   - That a fatal handshake failure is reported as failure rather than as a
//     stall, which is the difference between a closed connection and a leak.
template <class Backend>
void test_no_socket_handshake(const self_signed& cred)
{
    typename Backend::context tls;
    Backend::configure(tls, cred);
    tls.set_alpn({"h2", "http/1.1"});
    auto server_side = tls.make_transport();

    static const unsigned char offer[] = "\x08http/1.1\x02h2";
    offline_client c(offer, sizeof offer - 1);

    // Nothing to encrypt with yet, so this has to be queued rather than emitted
    std::string early;
    assert(server_side->app_out("ping", 4, early));
    assert(early.empty());
    assert(!server_side->established());

    std::string app_in;
    assert(c.run(*server_side, app_in));
    assert(server_side->established());
    assert(c.finished());
    assert(std::strcmp(server_side->alpn(), "h2") == 0);    // server preference, both backends
    assert(app_in.empty());                     // a handshake carries no application data

    // The queued plaintext went out with the last handshake flight
    char got[16] = {};
    assert(SSL_read(c.native(), got, int(sizeof got)) == 4);
    assert(std::memcmp(got, "ping", 4) == 0);

    // Garbage where a ClientHello should be is a fatal error, not a request for
    // more bytes: `false` is what tells the reactor to close the connection
    auto doomed = tls.make_transport();
    std::string junk_in, junk_out;
    const std::string junk(2048, '\xa5');
    assert(!doomed->wire_in(junk.data(), junk.size(), junk_in, junk_out));
    assert(!doomed->established());

    pass(tagged(Backend::label(), "offline handshake, server-side ALPN, queued plaintext").c_str());
}

// Credentials off the filesystem, which is how a real deployment loads them and
// which nothing else here touches. Worth its own test because the two backends
// take completely different routes to the same place - and because mbedTLS's
// in-memory parser needs the terminating NUL counted in the length while its
// file parser does not, so one path passing says nothing about the other.
template <class Backend>
void test_file_credentials(const self_signed& cred)
{
    char dir[] = "/tmp/tsmoveables_tls_XXXXXX";
    assert(::mkdtemp(dir) != nullptr);
    const std::string cert_path = std::string(dir) + "/cert.pem";
    const std::string key_path = std::string(dir) + "/key.pem";
    {
        std::ofstream(cert_path) << cred.cert_pem;
        std::ofstream(key_path) << cred.key_pem;
    }

    {
        typename Backend::context tls;
        tls.use_certificate_file(cert_path);
        tls.use_private_key_file(key_path);
        tls.set_alpn({"http/1.1"});

        auto server_side = tls.make_transport();
        offline_client c;
        std::string app_in;
        assert(c.run(*server_side, app_in));
        assert(server_side->established());

        // A missing file must be an exception, not a context that quietly has
        // no certificate and fails every handshake later
        assert(throws_runtime_error([&] { tls.use_certificate_file(std::string(dir) + "/nope"); }));
        assert(throws_runtime_error([&] { tls.use_private_key_file(std::string(dir) + "/nope"); }));
    }

    ::unlink(cert_path.c_str());
    ::unlink(key_path.c_str());
    ::rmdir(dir);

    pass(tagged(Backend::label(), "certificate and key loaded from files").c_str());
}

// The suite, once. Adding a third backend means adding a trait and one call.
template <class Backend>
void run_backend(const self_signed& cred)
{
    std::cout << "\n-- backend: " << Backend::label() << "\n";
    test_tls_request_response<Backend>(cred);
    test_tls_alpn<Backend>(cred);
    test_tls_large_body<Backend>(cred);
    test_wss<Backend>(cred);
    test_no_socket_handshake<Backend>(cred);
    test_file_credentials<Backend>(cred);
}

} // namespace

int main()
{
    self_signed cred;

    run_backend<openssl_backend>(cred);

#if defined(SNICHOLLS_TEST_MBEDTLS)
    run_backend<mbedtls_backend>(cred);
#else
    std::cout << "\n-- backend: mbedtls - not installed, skipped\n";
#endif

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
