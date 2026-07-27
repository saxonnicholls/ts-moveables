//
//  websocket_deflate.hpp
//  TSMoveables
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - permessage-deflate (RFC 7692) as an extension delegate
//
//  The third delegate axis, and it needed no new machinery: transport decides
//  how bytes arrive, protocol decides what they mean, and an extension
//  transforms the message payload. permessage-deflate is a byte transformer
//  with negotiated state - exactly the shape of the TLS engine - so it slots
//  into websocket.hpp without that header knowing zlib exists.
//
//      #include "websocket_deflate.hpp"
//
//      snicholls::http::ws_config ws;
//      ws.extension_factory = snicholls::http::deflate_factory();
//      srv.get("/chat", snicholls::http::websocket_route(on_open, ws));
//
//  Opt-in, like tls_openssl.hpp: this is the only WebSocket file that needs a
//  third party (zlib), and nothing includes it unless you do. Link with -lz.
//
//  RFC 7692 in three facts, all of which the code below turns on:
//    - a compressed message sets RSV1 on its *first* frame, not on every one;
//    - the payload is a raw DEFLATE stream with the trailing empty block
//      0x00 0x00 0xFF 0xFF removed, so decompressing means appending it back;
//    - by default the LZ77 window carries across messages ("context
//      takeover"), which compresses far better on chatty connections - unless
//      either side negotiates no_context_takeover, which resets per message.
//

#ifndef websocket_deflate_hpp
#define websocket_deflate_hpp

#include "websocket.hpp"

#if !SNICHOLLS_HAS_WEBSOCKET
#define SNICHOLLS_HAS_WS_DEFLATE 0
#else
#define SNICHOLLS_HAS_WS_DEFLATE 1

#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>

#include <zlib.h>

namespace snicholls {
namespace http {

class permessage_deflate final : public ws_extension {
public:
    struct options {
        int level = Z_DEFAULT_COMPRESSION;
        int mem_level = 8;
        // What this server is willing to do. Window bits below 9 are refused
        // rather than silently widened: zlib cannot do a raw 8-bit window, and
        // quietly using 9 while claiming 8 would be an interop lie.
        int max_window_bits = 15;
        bool allow_context_takeover = true;
        std::size_t max_decompressed = 16u * 1024 * 1024;   // decompression-bomb guard
        std::size_t min_size_to_compress = 64;              // tiny frames only grow
    };

    permessage_deflate() = default;
    explicit permessage_deflate(const options& o) : opt_(o) {}

    ~permessage_deflate() override
    {
        if (deflate_ready_) deflateEnd(&def_);
        if (inflate_ready_) inflateEnd(&inf_);
    }

    permessage_deflate(const permessage_deflate&) = delete;
    permessage_deflate& operator=(const permessage_deflate&) = delete;

    const char* name() const noexcept override { return "permessage-deflate"; }

    bool negotiate(const std::string& offer_params, std::string& response_params) override
    {
        int server_bits = opt_.max_window_bits;
        int client_bits = 15;
        bool client_offered_window = false;

        // Parse "; a; b=c" into decisions
        std::size_t i = 0;
        while (i <= offer_params.size()) {
            std::size_t semi = offer_params.find(';', i);
            if (semi == std::string::npos)
                semi = offer_params.size();
            std::string kv = offer_params.substr(i, semi - i);
            i = semi + 1;
            trim(kv);
            if (kv.empty())
                continue;

            std::string key = kv, value;
            const std::size_t eq = kv.find('=');
            if (eq != std::string::npos) {
                key = kv.substr(0, eq);
                value = kv.substr(eq + 1);
                trim(key);
                trim(value);
                if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                    value = value.substr(1, value.size() - 2);
            }

            if (key == "server_no_context_takeover") {
                server_no_takeover_ = true;
            } else if (key == "client_no_context_takeover") {
                client_no_takeover_ = true;
            } else if (key == "server_max_window_bits") {
                if (value.empty())
                    return false;               // must carry a value in an offer
                const int n = std::atoi(value.c_str());
                if (n < 8 || n > 15)
                    return false;
                if (n < 9)
                    return false;               // cannot honour it truthfully
                server_bits = (n < server_bits) ? n : server_bits;
            } else if (key == "client_max_window_bits") {
                client_offered_window = true;
                if (!value.empty()) {
                    const int n = std::atoi(value.c_str());
                    if (n < 8 || n > 15)
                        return false;
                    client_bits = n;
                }
            } else {
                return false;                   // an unknown parameter must be declined
            }
        }

        if (client_bits < 9)
            return false;
        if (!opt_.allow_context_takeover) {
            server_no_takeover_ = true;
            client_no_takeover_ = true;
        }

        server_bits_ = server_bits;
        client_bits_ = client_bits;

        response_params.clear();
        auto add = [&response_params](const std::string& p) {
            if (!response_params.empty())
                response_params += "; ";
            response_params += p;
        };
        if (server_no_takeover_) add("server_no_context_takeover");
        if (client_no_takeover_) add("client_no_context_takeover");
        if (server_bits_ != 15)  add("server_max_window_bits=" + std::to_string(server_bits_));
        // Only echo client_max_window_bits when the client offered it - saying
        // it unprompted is a protocol error on their side of the negotiation
        if (client_offered_window && client_bits_ != 15)
            add("client_max_window_bits=" + std::to_string(client_bits_));

        negotiated_ = true;
        return true;
    }

    bool compress(const std::string& in, std::string& out) override
    {
        if (!negotiated_)
            return false;
        if (in.size() < opt_.min_size_to_compress)
            return false;                       // not worth it: send it plain
        if (!deflate_ready_) {
            std::memset(&def_, 0, sizeof def_);
            if (deflateInit2(&def_, opt_.level, Z_DEFLATED, -server_bits_,
                             opt_.mem_level, Z_DEFAULT_STRATEGY) != Z_OK)
                return false;
            deflate_ready_ = true;
        }

        out.clear();
        out.reserve(in.size() / 2 + 64);
        def_.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
        def_.avail_in = uInt(in.size());
        char buf[16384];
        do {
            def_.next_out = reinterpret_cast<Bytef*>(buf);
            def_.avail_out = uInt(sizeof buf);
            const int rc = deflate(&def_, Z_SYNC_FLUSH);
            if (rc != Z_OK && rc != Z_BUF_ERROR)
                return false;
            out.append(buf, sizeof buf - def_.avail_out);
        } while (def_.avail_out == 0);

        // Strip the empty deflate block that Z_SYNC_FLUSH leaves behind: RFC
        // 7692 says the frame carries everything before it
        if (out.size() >= 4 &&
            std::memcmp(out.data() + out.size() - 4, "\x00\x00\xff\xff", 4) == 0)
            out.resize(out.size() - 4);

        if (server_no_takeover_)
            deflateReset(&def_);                // window does not carry to the next message
        return true;
    }

    bool decompress(const std::string& in, std::string& out) override
    {
        if (!negotiated_)
            return false;
        if (!inflate_ready_) {
            std::memset(&inf_, 0, sizeof inf_);
            if (inflateInit2(&inf_, -client_bits_) != Z_OK)
                return false;
            inflate_ready_ = true;
        }

        // Put back the block the sender stripped
        std::string framed = in;
        framed.append("\x00\x00\xff\xff", 4);

        out.clear();
        inf_.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(framed.data()));
        inf_.avail_in = uInt(framed.size());
        char buf[16384];
        for (;;) {
            inf_.next_out = reinterpret_cast<Bytef*>(buf);
            inf_.avail_out = uInt(sizeof buf);
            const int rc = inflate(&inf_, Z_NO_FLUSH);
            if (rc != Z_OK && rc != Z_BUF_ERROR && rc != Z_STREAM_END)
                return false;                   // corrupt: the caller closes with 1007
            out.append(buf, sizeof buf - inf_.avail_out);
            // A small compressed payload can expand enormously; refusing is
            // the whole point of the cap
            if (out.size() > opt_.max_decompressed)
                return false;
            if (inf_.avail_out != 0)
                break;
        }

        if (client_no_takeover_)
            inflateReset(&inf_);
        return true;
    }

private:
    static void trim(std::string& t)
    {
        const std::size_t b = t.find_first_not_of(" \t");
        const std::size_t e = t.find_last_not_of(" \t");
        t = (b == std::string::npos) ? std::string() : t.substr(b, e - b + 1);
    }

    options opt_{};
    z_stream def_{};
    z_stream inf_{};
    int server_bits_ = 15;
    int client_bits_ = 15;
    bool server_no_takeover_ = false;
    bool client_no_takeover_ = false;
    bool negotiated_ = false;
    bool deflate_ready_ = false;
    bool inflate_ready_ = false;
};

// Hand this to ws_config::extension_factory. One instance per connection,
// because the compression window is per connection and stateful.
inline std::function<std::unique_ptr<ws_extension>()>
deflate_factory(permessage_deflate::options o = permessage_deflate::options{})
{
    return [o] { return std::unique_ptr<ws_extension>(new permessage_deflate(o)); };
}

} // namespace http
} // namespace snicholls

#endif // SNICHOLLS_HAS_WEBSOCKET
#endif /* websocket_deflate_hpp */
