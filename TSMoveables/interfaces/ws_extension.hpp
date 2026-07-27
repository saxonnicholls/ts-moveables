//
//  interfaces/ws_extension.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  A WebSocket per-message extension (RFC 6455 §9) - the third delegate axis.
//
//  Transport decides how bytes arrive, protocol decides what they mean, and an
//  extension transforms the message payload. Like the transport interface this
//  needs nothing but <string>: it is a byte transformer with negotiated state,
//  so a backend can be written and tested without a socket in sight.
//
//  Shipped implementation: permessage-deflate, in http/websocket_deflate.hpp.
//

#ifndef interfaces_ws_extension_hpp
#define interfaces_ws_extension_hpp

#include <functional>
#include <memory>
#include <string>

namespace snicholls {
namespace http {

class ws_extension {
public:
    virtual ~ws_extension() = default;
    virtual const char* name() const noexcept = 0;

    // Given this extension's parameters from the client's offer, fill in the
    // parameters to send back and return true to accept. Returning false
    // declines the offer, and the connection proceeds uncompressed.
    virtual bool negotiate(const std::string& offer_params, std::string& response_params) = 0;

    virtual bool compress(const std::string& in, std::string& out) = 0;
    virtual bool decompress(const std::string& in, std::string& out) = 0;
};

} // namespace http
} // namespace snicholls

#endif /* interfaces_ws_extension_hpp */
