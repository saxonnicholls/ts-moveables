//
//  tls_mbedtls.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - TLS as a transport delegate, backed by mbedTLS
//
//  The second backend, and the reason it exists is to test a claim rather than
//  to add a feature. `tls_openssl.hpp` asserts that the engine is
//  transport-agnostic and that the backend is a run-time choice; with one
//  implementation that is an assertion, not a result. mbedTLS was picked
//  because it is built on a different idea to OpenSSL:
//
//    - OpenSSL has BIOs. You hand it a pair of memory BIOs and it moves bytes
//      into and out of them itself.
//    - mbedTLS has no such abstraction. You give it two function pointers -
//      `mbedtls_ssl_set_bio(ssl, ctx, send, recv, nullptr)` - and the library
//      calls *you* whenever it needs bytes. The callbacks return
//      MBEDTLS_ERR_SSL_WANT_READ / WANT_WRITE when they cannot proceed.
//
//  Inverted control flow, then, and the interesting outcome is that
//  `transport_delegate` did not need to change to accommodate it. Both shapes
//  reduce to the same four questions - here are wire bytes, give me plaintext;
//  here is plaintext, give me wire bytes; are we established; what did ALPN
//  choose - because the interface was written in terms of buffers the caller
//  owns rather than in terms of whatever the library calls its I/O layer.
//
//  Everything else follows `tls_openssl.hpp` exactly: no descriptor is ever
//  touched, nothing blocks, the reactor owns all I/O, and no code above knows
//  TLS exists. `https` is `http` with a different transport and `wss` is `ws`
//  with a different transport - now demonstrably so on two backends that share
//  no code.
//
//  This header is **opt-in and not part of the dependency-free core**: like the
//  OpenSSL one it needs a third-party library, and nothing includes it unless
//  you do. Both may be included in the same translation unit; they share no
//  symbols beyond the interface.
//
//      #include "mbedtls.hpp"
//
//      snicholls::http::mbedtls_context tls;
//      tls.use_certificate_file("cert.pem");
//      tls.use_private_key_file("key.pem");
//      tls.set_alpn({"http/1.1"});
//
//      snicholls::http::server srv;
//      srv.transport_factory(tls.factory());       // chosen at run time
//      srv.listen("0.0.0.0", 8443);
//
//  Build with, for example:
//      -I$(brew --prefix mbedtls)/include -L$(brew --prefix mbedtls)/lib \
//      -lmbedtls -lmbedx509 -lmbedcrypto
//
//  mbedTLS 3.x is assumed. The 2.x API differs enough (version constants,
//  `mbedtls_pk_parse_key` arity) that supporting both would cost more in
//  #ifdefs than it is worth here.
//

#ifndef tls_mbedtls_hpp
#define tls_mbedtls_hpp

#include "../http/server.hpp"

#if !SNICHOLLS_HAS_HTTP_SERVER
// Identical to the definition in tls_openssl.hpp on purpose: including both
// headers must not be a macro redefinition error, and it is not, because
// repeating an object-like macro with an identical replacement list is legal
#define SNICHOLLS_HAS_TLS 0
#define SNICHOLLS_HAS_TLS_MBEDTLS 0
#else
#define SNICHOLLS_HAS_TLS 1
#define SNICHOLLS_HAS_TLS_MBEDTLS 1

#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <mbedtls/build_info.h>                 // 3.x only; 2.x calls this version.h

#if MBEDTLS_VERSION_MAJOR < 3
#error "tls_mbedtls.hpp requires mbedTLS 3.x - the 2.x API differs in ways this file does not paper over"
#endif

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#if defined(MBEDTLS_PSA_CRYPTO_C)
#include <psa/crypto.h>
#endif

namespace snicholls {
namespace http {

namespace detail {

// mbedTLS returns one negative code per call rather than keeping an error
// queue, so unlike the OpenSSL side there is nothing to drain - the returned
// code is the whole story. The numeric value is kept because half of mbedTLS
// troubleshooting is grepping the headers for the constant.
inline std::string mbedtls_error(int rc)
{
    char text[192];
    mbedtls_strerror(rc, text, sizeof text);
    char num[32];
    std::snprintf(num, sizeof num, " (-0x%04X)", unsigned(-rc));
    return std::string(text) + num;
}

} // namespace detail

// ------------------------------------------------------------ the delegate

class mbedtls_transport final : public transport_delegate {
public:
    // Takes a configured server-side mbedtls_ssl_config, which must outlive
    // this object. mbedTLS does not reference-count its config the way OpenSSL
    // reference-counts SSL_CTX, so that lifetime is ours to honour: the
    // context outlives the server, and the server outlives its connections.
    explicit mbedtls_transport(const mbedtls_ssl_config* conf)
    {
        mbedtls_ssl_init(&ssl_);
        const int rc = mbedtls_ssl_setup(&ssl_, conf);
        if (rc != 0) {
            mbedtls_ssl_free(&ssl_);
            throw std::runtime_error("mbedtls_transport: ssl_setup: " + detail::mbedtls_error(rc));
        }
        // The whole backend difference in one call. There is no BIO to install
        // and no socket to hand over; mbedTLS calls these when it wants bytes,
        // and they move data to and from the two buffers below. The fourth slot
        // is the recv-with-timeout callback, which only DTLS needs.
        mbedtls_ssl_set_bio(&ssl_, this, &mbedtls_transport::bio_send,
                            &mbedtls_transport::bio_recv, nullptr);
        // No accept-state call to make: the endpoint is baked into the config,
        // so a server config can only ever produce server handshakes
    }

    ~mbedtls_transport() override { mbedtls_ssl_free(&ssl_); }

    mbedtls_transport(const mbedtls_transport&) = delete;
    mbedtls_transport& operator=(const mbedtls_transport&) = delete;

    const char* name() const noexcept override { return "mbedtls"; }
    bool established() const noexcept override { return established_; }

    const char* alpn() const noexcept override
    {
        return alpn_.c_str();               // empty means nothing was negotiated
    }

    bool wire_in(const char* data, std::size_t n,
                 std::string& app_in, std::string& wire_out) override
    {
        if (n)
            in_.append(data, n);

        if (!established_) {
            // One call, not one per flight: mbedtls_ssl_handshake runs the
            // state machine until it either finishes or needs I/O, at which
            // point our recv callback has already said WANT_READ for it
            const int rc = mbedtls_ssl_handshake(&ssl_);
            if (rc == 0) {
                established_ = true;
                capture_alpn();
                // Anything the application wrote while we were still shaking
                // hands has been held back; it can go now
                if (!pending_.empty()) {
                    const std::string queued = std::move(pending_);
                    pending_.clear();
                    if (!encrypt(queued.data(), queued.size())) {
                        drain(wire_out);
                        return false;
                    }
                }
            } else if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
                drain(wire_out);
                compact();
                return true;                    // more handshake bytes needed
            } else {
                drain(wire_out);                // let any alert reach the peer
                compact();
                return false;
            }
        }

        for (;;) {
            char buf[16384];
            const int rc = mbedtls_ssl_read(&ssl_, reinterpret_cast<unsigned char*>(buf),
                                            sizeof buf);
            if (rc > 0) {
                app_in.append(buf, std::size_t(rc));
                continue;
            }
            if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
                break;
            // Both spellings of a clean close: 3.x reports close_notify as its
            // own code, and 0 remains "the peer is done" for older records
            if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                peer_closed_ = true;
                break;
            }
            drain(wire_out);
            compact();
            return false;
        }

        drain(wire_out);
        compact();
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
        if (established_) {
            mbedtls_ssl_close_notify(&ssl_);    // best effort, like the OpenSSL side
            drain(wire_out);
        }
    }

    bool peer_closed() const noexcept { return peer_closed_; }

private:
    // ---- the two callbacks that replace OpenSSL's memory BIOs

    // Appending to a std::string cannot fail short of bad_alloc, so this never
    // returns WANT_WRITE - which is why the encrypt loop below can treat that
    // code as impossible rather than as a state to unwind
    static int bio_send(void* p, const unsigned char* buf, std::size_t len)
    {
        auto* self = static_cast<mbedtls_transport*>(p);
        self->out_.append(reinterpret_cast<const char*>(buf), len);
        return int(len);
    }

    // WANT_READ, never 0. This is the exact counterpart of the OpenSSL side's
    // BIO_set_mem_eof_return(rbio_, -1): an empty buffer means "no data yet",
    // not "end of stream", because the reactor will bring more when the socket
    // says so. Returning 0 here would abort the handshake instead of pausing it.
    static int bio_recv(void* p, unsigned char* buf, std::size_t len)
    {
        auto* self = static_cast<mbedtls_transport*>(p);
        const std::size_t have = self->in_.size() - self->in_pos_;
        if (have == 0)
            return MBEDTLS_ERR_SSL_WANT_READ;
        const std::size_t take = have < len ? have : len;
        std::memcpy(buf, self->in_.data() + self->in_pos_, take);
        self->in_pos_ += take;
        return int(take);
    }

    bool encrypt(const char* data, std::size_t n)
    {
        std::size_t off = 0;
        while (off < n) {
            // Short writes are normal: one call emits at most one record, so a
            // 512 KiB body becomes a loop rather than one enormous frame
            const int rc = mbedtls_ssl_write(&ssl_,
                                             reinterpret_cast<const unsigned char*>(data) + off,
                                             n - off);
            if (rc > 0) {
                off += std::size_t(rc);
                continue;
            }
            // WANT_WRITE would oblige us to retry with the identical pointer
            // and length, which this loop does satisfy - but bio_send cannot
            // produce it, so reaching here means something is wrong and
            // dropping plaintext silently would be worse than closing
            return false;
        }
        return true;
    }

    void drain(std::string& wire_out)
    {
        if (out_.empty())
            return;
        wire_out.append(out_);
        out_.clear();
        if (out_.capacity() > 256u * 1024)
            std::string().swap(out_);
    }

    // Ciphertext mbedTLS has consumed is dead weight; drop it once per event
    // rather than per callback, so a partially consumed record is not copied
    // down on every read of it
    void compact()
    {
        if (in_pos_ == in_.size()) {
            in_.clear();
            in_pos_ = 0;
            if (in_.capacity() > 256u * 1024)
                std::string().swap(in_);
        } else if (in_pos_) {
            in_.erase(0, in_pos_);
            in_pos_ = 0;
        }
    }

    void capture_alpn()
    {
        // The returned pointer aims into the config's protocol list, which
        // outlives us; copying anyway keeps alpn()'s lifetime obvious and
        // matches what the OpenSSL backend hands back
        if (const char* p = mbedtls_ssl_get_alpn_protocol(&ssl_))
            alpn_.assign(p);
    }

    mbedtls_ssl_context ssl_;
    std::string in_;                            // ciphertext handed to us
    std::size_t in_pos_ = 0;                    // how much of it mbedTLS has taken
    std::string out_;                           // ciphertext mbedTLS handed back
    std::string pending_;                       // plaintext written pre-handshake
    std::string alpn_;
    bool established_ = false;
    bool peer_closed_ = false;
};

// ------------------------------------------------------------- the context
//
// One config shared by every connection; each connection gets its own
// mbedtls_ssl_context. Moveable, like everything else here.
//
// The mbedTLS state lives behind a unique_ptr rather than inline, and that is
// not an ownership habit - it is a correctness requirement. An
// mbedtls_ssl_context stores a bare `const mbedtls_ssl_config*`, and the config
// in turn stores bare pointers to the certificate, the key and the RNG. Move a
// context that held those by value and every live connection would be pointing
// at the corpse. One indirection makes the addresses stable, and the move
// operator can then stay defaulted and trivially correct.

class mbedtls_context {
public:
    mbedtls_context() : st_(new state)
    {
#if defined(MBEDTLS_PSA_CRYPTO_C)
        // TLS 1.3 runs its own cryptography through PSA whatever the build
        // options say, and mbedTLS would otherwise initialise it lazily inside
        // the first handshake. Doing it here means a failure surfaces at
        // configuration time on the calling thread, not mid-connection.
        if (psa_crypto_init() != PSA_SUCCESS)
            throw std::runtime_error("mbedtls_context: psa_crypto_init failed");
#endif
        int rc = mbedtls_ctr_drbg_seed(&st_->drbg, mbedtls_entropy_func, &st_->entropy,
                                       reinterpret_cast<const unsigned char*>(kSeed),
                                       sizeof kSeed - 1);
        if (rc != 0)
            throw std::runtime_error("mbedtls_context: ctr_drbg_seed: " + detail::mbedtls_error(rc));

        rc = mbedtls_ssl_config_defaults(&st_->conf, MBEDTLS_SSL_IS_SERVER,
                                         MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT);
        if (rc != 0)
            throw std::runtime_error("mbedtls_context: config_defaults: " +
                                     detail::mbedtls_error(rc));

        mbedtls_ssl_conf_rng(&st_->conf, mbedtls_ctr_drbg_random, &st_->drbg);
        // TLS 1.2 is the floor; anything older is a liability, not a feature
        mbedtls_ssl_conf_min_tls_version(&st_->conf, MBEDTLS_SSL_VERSION_TLS1_2);
        // No client certificate is asked for, which is the server default in
        // mbedTLS but is stated because the client-side default is the opposite
        mbedtls_ssl_conf_authmode(&st_->conf, MBEDTLS_SSL_VERIFY_NONE);
    }

    mbedtls_context(mbedtls_context&&) noexcept = default;
    mbedtls_context& operator=(mbedtls_context&&) noexcept = default;

    void use_certificate_file(const std::string& path)
    {
        const int rc = mbedtls_x509_crt_parse_file(&st_->cert, path.c_str());
        if (rc != 0)
            throw std::runtime_error("mbedtls_context: certificate " + path + ": " +
                                     detail::mbedtls_error(rc));
        st_->have_cert = true;
        attach();
    }

    void use_private_key_file(const std::string& path)
    {
        // The RNG is not optional here even though nothing random is being
        // read: mbedTLS blinds RSA operations, and the key check does one
        const int rc = mbedtls_pk_parse_keyfile(&st_->key, path.c_str(), nullptr,
                                                mbedtls_ctr_drbg_random, &st_->drbg);
        if (rc != 0)
            throw std::runtime_error("mbedtls_context: private key " + path + ": " +
                                     detail::mbedtls_error(rc));
        st_->have_key = true;
        attach();
    }

    // For credentials held in memory - a generated development pair, or a
    // secret fetched at start-up that should never touch the filesystem. PEM
    // text, which is what both mbedTLS and OpenSSL will hand you.
    //
    // The +1 on each length is not a typo and is the single most common way to
    // get MBEDTLS_ERR_PEM_NO_HEADER_FOOTER: mbedTLS decides PEM versus DER by
    // looking for a terminating NUL, so for PEM the length must count it.
    void use_certificate_and_key(const std::string& cert_pem, const std::string& key_pem)
    {
        int rc = mbedtls_x509_crt_parse(&st_->cert,
                                        reinterpret_cast<const unsigned char*>(cert_pem.c_str()),
                                        cert_pem.size() + 1);
        if (rc != 0)
            throw std::runtime_error("mbedtls_context: in-memory certificate: " +
                                     detail::mbedtls_error(rc));
        st_->have_cert = true;

        rc = mbedtls_pk_parse_key(&st_->key,
                                  reinterpret_cast<const unsigned char*>(key_pem.c_str()),
                                  key_pem.size() + 1, nullptr, 0,
                                  mbedtls_ctr_drbg_random, &st_->drbg);
        if (rc != 0)
            throw std::runtime_error("mbedtls_context: in-memory private key: " +
                                     detail::mbedtls_error(rc));
        st_->have_key = true;
        attach();
    }

    // Protocols this server is willing to speak, most preferred first. mbedTLS
    // walks the server list outermost, so this order wins - the same server
    // preference the OpenSSL callback implements by hand.
    void set_alpn(std::vector<std::string> protocols)
    {
        // mbedTLS keeps the array by pointer for the life of the config and
        // never copies it, so the storage has to be ours and has to be stable.
        // Built in full before any c_str() is taken, because growing the vector
        // afterwards would move every string it holds.
        st_->alpn_names = std::move(protocols);
        st_->alpn_ptrs.clear();
        st_->alpn_ptrs.reserve(st_->alpn_names.size() + 1);
        for (const auto& p : st_->alpn_names)
            st_->alpn_ptrs.push_back(p.c_str());
        st_->alpn_ptrs.push_back(nullptr);      // the list is NULL-terminated

        const int rc = mbedtls_ssl_conf_alpn_protocols(
            &st_->conf, st_->alpn_names.empty() ? nullptr : st_->alpn_ptrs.data());
        if (rc != 0)
            throw std::runtime_error("mbedtls_context: set_alpn: " + detail::mbedtls_error(rc));
    }

    mbedtls_ssl_config* native() noexcept { return &st_->conf; }

    std::unique_ptr<transport_delegate> make_transport()
    {
        return std::unique_ptr<transport_delegate>(new mbedtls_transport(&st_->conf));
    }

    // Hand this to server::transport_factory() and the server speaks HTTPS.
    // Byte-for-byte the same call site as the OpenSSL context, which is the
    // point: the backend is a run-time choice and nothing above it changes.
    std::function<std::unique_ptr<transport_delegate>()> factory()
    {
        return [this] { return make_transport(); };
    }

private:
    static constexpr char kSeed[] = "snicholls-tsmoveables-tls";

    // mbedtls_ssl_conf_own_cert appends to a list, so it must be called exactly
    // once and only when both halves have arrived - and the two loaders can be
    // called in either order
    void attach()
    {
        if (!st_->have_cert || !st_->have_key || st_->attached)
            return;
        const int rc = mbedtls_ssl_conf_own_cert(&st_->conf, &st_->cert, &st_->key);
        if (rc != 0)
            throw std::runtime_error("mbedtls_context: conf_own_cert: " +
                                     detail::mbedtls_error(rc));
        st_->attached = true;
    }

    // Plain C structs with hand-rolled init/free, kept together so the order is
    // written down once. Neither copyable nor moveable on purpose: the whole
    // reason it is heap-allocated is that its address must not change.
    struct state {
        mbedtls_entropy_context entropy;
        mbedtls_ctr_drbg_context drbg;
        mbedtls_ssl_config conf;
        mbedtls_x509_crt cert;
        mbedtls_pk_context key;
        std::vector<std::string> alpn_names;
        std::vector<const char*> alpn_ptrs;
        bool have_cert = false;
        bool have_key = false;
        bool attached = false;

        state()
        {
            mbedtls_entropy_init(&entropy);
            mbedtls_ctr_drbg_init(&drbg);
            mbedtls_ssl_config_init(&conf);
            mbedtls_x509_crt_init(&cert);
            mbedtls_pk_init(&key);
        }

        ~state()
        {
            // Reverse order: the config points at the certificate, the key and
            // the DRBG, and the DRBG points at the entropy source
            mbedtls_pk_free(&key);
            mbedtls_x509_crt_free(&cert);
            mbedtls_ssl_config_free(&conf);
            mbedtls_ctr_drbg_free(&drbg);
            mbedtls_entropy_free(&entropy);
        }

        state(const state&) = delete;
        state& operator=(const state&) = delete;
    };

    std::unique_ptr<state> st_;
};

} // namespace http
} // namespace snicholls

#endif // SNICHOLLS_HAS_HTTP_SERVER
#endif /* tls_mbedtls_hpp */
