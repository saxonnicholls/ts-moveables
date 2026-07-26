//
//  tls_openssl.hpp
//  TSMoveables
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - TLS as a transport delegate, backed by OpenSSL
//
//  This is the other axis. A protocol delegate turns bytes into requests; a
//  transport delegate decides how those bytes arrive. Here that is TLS, and
//  the engine is deliberately **transport-agnostic**: OpenSSL is driven
//  through a pair of memory BIOs, so it never sees a socket, never calls
//  read() or write(), and never blocks. The reactor owns all I/O; this is a
//  byte transformer with a handshake.
//
//  Three things follow from that, and they are the whole argument for the
//  shape:
//
//    - Every backend becomes testable without a network. Ciphertext in,
//      plaintext out is a pure function of state.
//    - The WANT_READ / WANT_WRITE dance is explicit rather than hidden inside
//      a blocking socket call, which is what makes non-blocking TLS tractable.
//    - Nothing above knows TLS exists. `https` is `http` with a different
//      transport, and `wss` is `ws` with a different transport - no code in
//      either protocol delegate changes, because the axes never meet.
//
//  This header is **opt-in and not part of the dependency-free core**: it is
//  the only file in the library that needs a third-party library, and nothing
//  includes it unless you do.
//
//      #include "tls_openssl.hpp"
//
//      snicholls::http::openssl_context tls;
//      tls.use_certificate_file("cert.pem");
//      tls.use_private_key_file("key.pem");
//      tls.set_alpn({"http/1.1"});
//
//      snicholls::http::server srv;
//      srv.transport_factory(tls.factory());       // chosen at run time
//      srv.listen("0.0.0.0", 8443);
//
//  Build with, for example:
//      -I$(brew --prefix openssl@3)/include -L$(brew --prefix openssl@3)/lib -lssl -lcrypto
//
//  ALPN is answered here because that is where it belongs: the transport
//  learns which protocol the client asked for during the handshake, and a
//  later phase uses it to pick the protocol delegate (h2 versus http/1.1).
//  Reporting it now costs nothing and is what makes that possible without a
//  redesign.
//

#ifndef tls_openssl_hpp
#define tls_openssl_hpp

#include "http_server.hpp"

#if !SNICHOLLS_HAS_HTTP_SERVER
#define SNICHOLLS_HAS_TLS 0
#else
#define SNICHOLLS_HAS_TLS 1

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace snicholls {
namespace http {

namespace detail {

inline std::string openssl_error()
{
    std::string out;
    unsigned long e;
    char buf[256];
    while ((e = ERR_get_error()) != 0) {
        ERR_error_string_n(e, buf, sizeof buf);
        if (!out.empty())
            out += "; ";
        out += buf;
    }
    return out.empty() ? std::string("unknown OpenSSL error") : out;
}

} // namespace detail

// ------------------------------------------------------------ the delegate

class openssl_transport final : public transport_delegate {
public:
    // Takes an SSL* already configured for server-side accept
    explicit openssl_transport(SSL* ssl) : ssl_(ssl)
    {
        // Memory BIOs both ways: OpenSSL reads ciphertext we hand it and
        // writes ciphertext we collect, and never touches a descriptor
        rbio_ = BIO_new(BIO_s_mem());
        wbio_ = BIO_new(BIO_s_mem());
        BIO_set_mem_eof_return(rbio_, -1);      // "no data yet", not "end of stream"
        BIO_set_mem_eof_return(wbio_, -1);
        SSL_set_bio(ssl_, rbio_, wbio_);        // SSL takes ownership of both
        SSL_set_accept_state(ssl_);
    }

    ~openssl_transport() override
    {
        if (ssl_)
            SSL_free(ssl_);                     // frees the BIOs with it
    }

    openssl_transport(const openssl_transport&) = delete;
    openssl_transport& operator=(const openssl_transport&) = delete;

    const char* name() const noexcept override { return "openssl"; }
    bool established() const noexcept override { return established_; }

    const char* alpn() const noexcept override
    {
        return alpn_.empty() ? "http/1.1" : alpn_.c_str();
    }

    bool wire_in(const char* data, std::size_t n,
                 std::string& app_in, std::string& wire_out) override
    {
        if (n && BIO_write(rbio_, data, int(n)) <= 0)
            return false;

        if (!established_) {
            const int r = SSL_accept(ssl_);
            if (r == 1) {
                established_ = true;
                capture_alpn();
                // Anything the application wrote while we were still shaking
                // hands has been held back; it can go now
                if (!pending_.empty()) {
                    const std::string queued = std::move(pending_);
                    pending_.clear();
                    if (!encrypt(queued.data(), queued.size()))
                        return false;
                }
            } else {
                const int err = SSL_get_error(ssl_, r);
                if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                    drain(wire_out);            // let any alert reach the peer
                    return false;
                }
                drain(wire_out);
                return true;                    // more handshake bytes needed
            }
        }

        for (;;) {
            char buf[16384];
            const int r = SSL_read(ssl_, buf, int(sizeof buf));
            if (r > 0) {
                app_in.append(buf, std::size_t(r));
                continue;
            }
            const int err = SSL_get_error(ssl_, r);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
                break;
            if (err == SSL_ERROR_ZERO_RETURN) { // clean close_notify from the peer
                peer_closed_ = true;
                break;
            }
            drain(wire_out);
            return false;
        }

        drain(wire_out);
        return true;
    }

    bool app_out(const char* data, std::size_t n, std::string& wire_out) override
    {
        if (!established_) {
            pending_.append(data, n);           // cannot encrypt before the handshake
            return true;
        }
        if (!encrypt(data, n))
            return false;
        drain(wire_out);
        return true;
    }

    void shutdown(std::string& wire_out) override
    {
        if (ssl_ && established_) {
            SSL_shutdown(ssl_);                 // best effort close_notify
            drain(wire_out);
        }
    }

    bool peer_closed() const noexcept { return peer_closed_; }

private:
    bool encrypt(const char* data, std::size_t n)
    {
        std::size_t off = 0;
        while (off < n) {
            const int r = SSL_write(ssl_, data + off, int(n - off));
            if (r > 0) {
                off += std::size_t(r);
                continue;
            }
            const int err = SSL_get_error(ssl_, r);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
                return true;                    // a memory BIO should not do this, but say so safely
            return false;
        }
        return true;
    }

    void drain(std::string& wire_out)
    {
        char buf[16384];
        for (;;) {
            const int r = BIO_read(wbio_, buf, int(sizeof buf));
            if (r <= 0)
                break;
            wire_out.append(buf, std::size_t(r));
        }
    }

    void capture_alpn()
    {
        const unsigned char* p = nullptr;
        unsigned len = 0;
        SSL_get0_alpn_selected(ssl_, &p, &len);
        if (p && len)
            alpn_.assign(reinterpret_cast<const char*>(p), len);
    }

    SSL* ssl_ = nullptr;
    BIO* rbio_ = nullptr;
    BIO* wbio_ = nullptr;
    std::string pending_;                       // plaintext written pre-handshake
    std::string alpn_;
    bool established_ = false;
    bool peer_closed_ = false;
};

// ------------------------------------------------------------- the context
//
// One SSL_CTX shared by every connection; each connection gets its own SSL.
// Moveable, like everything else here, so a configured context can be built
// in a factory and moved into place.

class openssl_context {
public:
    openssl_context()
    {
        ctx_.reset(SSL_CTX_new(TLS_server_method()));
        if (!ctx_)
            throw std::runtime_error("openssl_context: SSL_CTX_new: " + detail::openssl_error());
        // TLS 1.2 is the floor; anything older is a liability, not a feature
        SSL_CTX_set_min_proto_version(ctx_.get(), TLS1_2_VERSION);
        SSL_CTX_set_options(ctx_.get(), SSL_OP_NO_COMPRESSION | SSL_OP_CIPHER_SERVER_PREFERENCE);
        // Retrying a write with a moved buffer is normal for a reactor that
        // owns its own buffers, and partial writes are expected
        SSL_CTX_set_mode(ctx_.get(), SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER |
                                     SSL_MODE_ENABLE_PARTIAL_WRITE);
    }

    openssl_context(openssl_context&&) noexcept = default;
    openssl_context& operator=(openssl_context&&) noexcept = default;

    void use_certificate_file(const std::string& path)
    {
        if (SSL_CTX_use_certificate_chain_file(ctx_.get(), path.c_str()) != 1)
            throw std::runtime_error("openssl_context: certificate " + path + ": " +
                                     detail::openssl_error());
    }

    void use_private_key_file(const std::string& path)
    {
        if (SSL_CTX_use_PrivateKey_file(ctx_.get(), path.c_str(), SSL_FILETYPE_PEM) != 1)
            throw std::runtime_error("openssl_context: private key " + path + ": " +
                                     detail::openssl_error());
        if (SSL_CTX_check_private_key(ctx_.get()) != 1)
            throw std::runtime_error("openssl_context: key does not match certificate");
    }

    // For a certificate held in memory - a generated development one, or a
    // secret fetched at start-up that should never touch the filesystem
    void use_certificate_and_key(X509* cert, EVP_PKEY* key)
    {
        if (SSL_CTX_use_certificate(ctx_.get(), cert) != 1 ||
            SSL_CTX_use_PrivateKey(ctx_.get(), key) != 1 ||
            SSL_CTX_check_private_key(ctx_.get()) != 1)
            throw std::runtime_error("openssl_context: in-memory credentials: " +
                                     detail::openssl_error());
    }

    // Protocols this server is willing to speak, most preferred first. The
    // handshake reports the winner, which is how a later phase picks between
    // h2 and http/1.1 per connection rather than per build.
    void set_alpn(std::vector<std::string> protocols)
    {
        alpn_.clear();
        for (const auto& p : protocols) {
            alpn_.push_back(static_cast<unsigned char>(p.size()));
            alpn_.insert(alpn_.end(), p.begin(), p.end());
        }
        SSL_CTX_set_alpn_select_cb(ctx_.get(), &alpn_select, this);
    }

    SSL_CTX* native() noexcept { return ctx_.get(); }

    std::unique_ptr<transport_delegate> make_transport()
    {
        SSL* ssl = SSL_new(ctx_.get());
        if (!ssl)
            throw std::runtime_error("openssl_context: SSL_new: " + detail::openssl_error());
        return std::unique_ptr<transport_delegate>(new openssl_transport(ssl));
    }

    // Hand this to server::transport_factory() and the server speaks HTTPS.
    // Nothing else about the server changes - that is the point.
    std::function<std::unique_ptr<transport_delegate>()> factory()
    {
        return [this] { return make_transport(); };
    }

private:
    static int alpn_select(SSL*, const unsigned char** out, unsigned char* outlen,
                           const unsigned char* in, unsigned int inlen, void* arg)
    {
        auto* self = static_cast<openssl_context*>(arg);
        if (self->alpn_.empty())
            return SSL_TLSEXT_ERR_NOACK;
        // Server preference order, which is the safer choice
        for (std::size_t i = 0; i + 1 <= self->alpn_.size();) {
            const unsigned char len = self->alpn_[i];
            const unsigned char* mine = self->alpn_.data() + i + 1;
            for (unsigned int j = 0; j + 1 <= inlen;) {
                const unsigned char clen = in[j];
                if (clen == len && std::memcmp(mine, in + j + 1, len) == 0) {
                    *out = in + j + 1;
                    *outlen = clen;
                    return SSL_TLSEXT_ERR_OK;
                }
                j += 1u + clen;
            }
            i += 1u + len;
        }
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    }

    struct ctx_deleter {
        void operator()(SSL_CTX* c) const noexcept
        {
            if (c)
                SSL_CTX_free(c);
        }
    };

    std::unique_ptr<SSL_CTX, ctx_deleter> ctx_;
    std::vector<unsigned char> alpn_;
};

} // namespace http
} // namespace snicholls

#endif // SNICHOLLS_HAS_HTTP_SERVER
#endif /* tls_openssl_hpp */
