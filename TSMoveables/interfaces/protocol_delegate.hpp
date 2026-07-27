//
//  interfaces/protocol_delegate.hpp
//  TSMoveables
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  How bytes become requests - the second of the two axes.
//
//  A protocol delegate owns a connection's bytes: HTTP/1.1, WebSocket after an
//  Upgrade, HTTP/2 after ALPN. connection_host is the other half of the
//  contract - what a delegate may ask of the connection it is running on.
//
//  The stream id on every method is always 0 for HTTP/1.x and exists so that
//  multiplexed protocols need no interface change. It was paid for before
//  HTTP/2 was written, and HTTP/2 needed nothing added.
//

#ifndef interfaces_protocol_delegate_hpp
#define interfaces_protocol_delegate_hpp

#include "../http/message.hpp"
#include "../http/config.hpp"

#include <cstdint>
#include <memory>

namespace snicholls {
namespace http {

// ------------------------------------------------------------------ delegates
//
// Transport: how bytes reach us. Plaintext today; a TLS engine is the same
// shape (ciphertext in, plaintext out) because it never touches the socket -
// the reactor owns all IO. That is what makes every backend testable without
// a network, and what makes the backend a run-time choice.

// transport_delegate and plain_transport now live in
// interfaces/transport_delegate.hpp - implementing a transport should not
// mean including the whole server.

class protocol_delegate;

// What a protocol delegate is allowed to ask of its connection
class connection_host {
public:
    virtual ~connection_host() = default;
    virtual void deliver(request& req, std::uint64_t stream) = 0;
    virtual void write_app(const char* data, std::size_t n) = 0;
    virtual void protocol_failure(int status, const char* reason) = 0;
    virtual const server_config& config() const noexcept = 0;
    virtual const char* http_date() = 0;
    virtual bool live() const noexcept = 0;

    // What ALPN settled on for this connection, or an empty string when
    // nothing did. A protocol delegate needs this to choose its successor:
    // "the client asked for h2" and "the client said nothing" are different
    // answers, and only the transport knows which one happened.
    virtual const char* alpn() const noexcept { return ""; }

    // Hand this connection to a different protocol - the Upgrade path. The
    // swap is deferred until the current delegate's consume() has returned,
    // because a delegate cannot safely be destroyed from inside its own call.
    virtual void switch_protocol(std::unique_ptr<protocol_delegate> next) = 0;
};

// Protocol: how bytes become requests. The stream id is always 0 for
// HTTP/1.x and exists so h2 and h3 need no interface change.
class protocol_delegate {
public:
    virtual ~protocol_delegate() = default;
    virtual const char* name() const noexcept = 0;
    virtual bool consume(std::string& in, connection_host& host) = 0;
    virtual void respond(std::uint64_t stream, response&& res, connection_host& host) = 0;
    virtual bool close_after_flush() const noexcept = 0;
    virtual bool busy() const noexcept = 0;         // a request is awaiting its response
    virtual bool receiving() const noexcept = 0;    // a partial request is buffered (slowloris)
    virtual std::size_t last_response_bytes() const noexcept { return 0; }

    // Streamed responses: headers now, body in pieces, length unknown at the
    // time of the headers. Without this a response must be complete in memory
    // before any of it is sent, which is fine for JSON and hopeless for a
    // file. Protocols that cannot stream leave begin_stream returning false.
    virtual bool begin_stream(std::uint64_t /*stream*/, response&& /*headers*/,
                              connection_host& /*host*/) { return false; }
    virtual void stream_write(std::uint64_t /*stream*/, const char* /*data*/,
                              std::size_t /*n*/, connection_host& /*host*/) {}
    virtual void end_stream(std::uint64_t /*stream*/, connection_host& /*host*/) {}
};

} // namespace http
} // namespace snicholls

#endif /* interfaces_protocol_delegate_hpp */
