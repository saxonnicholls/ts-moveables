//
//  interfaces/transport_delegate.hpp
//  TSMoveables
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  How bytes reach a connection - one of the two axes the server hangs on.
//
//  This is here, on its own, because it is an *extension point*: writing a TLS
//  backend means implementing this and nothing else. It needs only <string>,
//  because the engine is deliberately transport-agnostic - ciphertext in,
//  plaintext out, and the reverse. It never sees a socket; the reactor owns
//  all IO. That is what makes every backend testable without a network, and
//  it is why implementing one does not mean including a 2,700-line server.
//
//  Shipped implementations: plain (below), tls/openssl.hpp, tls/mbedtls.hpp.
//

#ifndef interfaces_transport_delegate_hpp
#define interfaces_transport_delegate_hpp

#include <cstddef>
#include <string>

namespace snicholls {
namespace http {

class transport_delegate {
public:
    virtual ~transport_delegate() = default;
    virtual const char* name() const noexcept = 0;

    // Wire bytes arrived: append application bytes to app_in, and any bytes
    // the transport itself owes the peer (handshake, alerts) to wire_out
    virtual bool wire_in(const char* data, std::size_t n,
                         std::string& app_in, std::string& wire_out) = 0;

    // The application wants to send bytes: append wire bytes to wire_out
    virtual bool app_out(const char* data, std::size_t n, std::string& wire_out) = 0;

    virtual bool established() const noexcept { return true; }
    // The protocol ALPN settled on, or an **empty string** when nothing was
    // negotiated - a plaintext connection, or a client that offered no list.
    // Those two cases must stay distinguishable: choosing the protocol
    // delegate by ALPN means "the client asked for h2" and "the client said
    // nothing, so default to http/1.1" are different answers, and collapsing
    // them into the literal "http/1.1" throws away the only bit that matters.
    virtual const char* alpn() const noexcept { return ""; }
    virtual void shutdown(std::string& /*wire_out*/) {}
};

class plain_transport final : public transport_delegate {
public:
    const char* name() const noexcept override { return "plain"; }

    bool wire_in(const char* data, std::size_t n, std::string& app_in, std::string&) override
    {
        app_in.append(data, n);
        return true;
    }

    bool app_out(const char* data, std::size_t n, std::string& wire_out) override
    {
        wire_out.append(data, n);
        return true;
    }
};

} // namespace http
} // namespace snicholls

#endif /* interfaces_transport_delegate_hpp */
