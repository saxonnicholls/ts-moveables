//
//  websocket.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - WebSocket (RFC 6455) as a protocol delegate
//
//  This is the delegate architecture's cleanest win. A WebSocket connection
//  *is* an HTTP connection that changed protocol mid-flight, so here it is one
//  call: the handler answers the Upgrade request with responder::upgrade(),
//  the http1 delegate hands over, and a websocket_protocol owns every byte
//  that follows. The transport underneath never knew - which is why `wss://`
//  needs no work at all: it is this over the TLS transport delegate, and the
//  two axes never meet.
//
//      srv.get("/chat", snicholls::http::websocket_route(
//          [](snicholls::http::websocket ws) {
//              ws.on_message([](auto sock, const auto& m) {
//                  sock.send_text("echo: " + m.data);
//              });
//          }));
//
//  Correctness the RFC insists on, and Autobahn checks: client frames must be
//  masked and unmasked ones are a protocol error; server frames must not be;
//  control frames are never fragmented and never exceed 125 bytes; text
//  payloads must be valid UTF-8; and close is a two-way handshake with a
//  status code. All of that is enforced here rather than assumed.
//
//  Dependency note: the handshake needs SHA-1 and base64 for one 20-byte
//  digest. Both are implemented here in about eighty lines rather than pulled
//  in, because this library's promise is that the core has no dependencies -
//  base-encode-decode remains the right tool for application-level encoding.
//  The SHA-1 here is a fixed handshake ritual, not a security primitive.
//

#ifndef websocket_hpp
#define websocket_hpp

#include "server.hpp"
#include "../interfaces/ws_extension.hpp"

#if !SNICHOLLS_HAS_HTTP_SERVER
#define SNICHOLLS_HAS_WEBSOCKET 0
#else
#define SNICHOLLS_HAS_WEBSOCKET 1

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace snicholls {
namespace http {

namespace detail {

// ------------------------------------------------------------------- SHA-1
// RFC 3174. Present only because RFC 6455 specifies it for the accept key.

inline std::array<unsigned char, 20> sha1(const std::string& in)
{
    auto rol = [](std::uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};

    std::string msg = in;
    const std::uint64_t bits = std::uint64_t(in.size()) * 8;
    msg.push_back('\x80');
    while (msg.size() % 64 != 56)
        msg.push_back('\0');
    for (int i = 7; i >= 0; --i)
        msg.push_back(char((bits >> (i * 8)) & 0xff));

    for (std::size_t off = 0; off < msg.size(); off += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const unsigned char* p = reinterpret_cast<const unsigned char*>(msg.data()) + off + i * 4;
            w[i] = (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
                   (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
        }
        for (int i = 16; i < 80; ++i)
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);            k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                     k = 0xCA62C1D6u; }
            const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    std::array<unsigned char, 20> out{};
    for (int i = 0; i < 5; ++i) {
        out[i * 4 + 0] = static_cast<unsigned char>((h[i] >> 24) & 0xff);
        out[i * 4 + 1] = static_cast<unsigned char>((h[i] >> 16) & 0xff);
        out[i * 4 + 2] = static_cast<unsigned char>((h[i] >> 8) & 0xff);
        out[i * 4 + 3] = static_cast<unsigned char>(h[i] & 0xff);
    }
    return out;
}

inline std::string base64(const unsigned char* p, std::size_t n)
{
    static const char* const t =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 2 < n; i += 3) {
        const std::uint32_t v = (std::uint32_t(p[i]) << 16) | (std::uint32_t(p[i + 1]) << 8) | p[i + 2];
        out.push_back(t[(v >> 18) & 63]);
        out.push_back(t[(v >> 12) & 63]);
        out.push_back(t[(v >> 6) & 63]);
        out.push_back(t[v & 63]);
    }
    if (i < n) {
        std::uint32_t v = std::uint32_t(p[i]) << 16;
        const bool two = (i + 1 < n);
        if (two)
            v |= std::uint32_t(p[i + 1]) << 8;
        out.push_back(t[(v >> 18) & 63]);
        out.push_back(t[(v >> 12) & 63]);
        out.push_back(two ? t[(v >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

// RFC 3629 - rejecting overlong forms, surrogates and out-of-range values,
// all of which Autobahn tests specifically
inline bool valid_utf8(const std::string& s)
{
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    std::size_t i = 0, n = s.size();
    while (i < n) {
        const unsigned char c = p[i];
        std::size_t extra;
        std::uint32_t cp;
        if (c < 0x80)                    { ++i; continue; }
        else if ((c & 0xE0) == 0xC0)     { extra = 1; cp = c & 0x1Fu; }
        else if ((c & 0xF0) == 0xE0)     { extra = 2; cp = c & 0x0Fu; }
        else if ((c & 0xF8) == 0xF0)     { extra = 3; cp = c & 0x07u; }
        else                             return false;

        if (i + extra >= n)
            return false;
        for (std::size_t k = 1; k <= extra; ++k) {
            const unsigned char cc = p[i + k];
            if ((cc & 0xC0) != 0x80)
                return false;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (extra == 1 && cp < 0x80)        return false;   // overlong
        if (extra == 2 && cp < 0x800)       return false;
        if (extra == 3 && cp < 0x10000)     return false;
        if (cp > 0x10FFFF)                  return false;
        if (cp >= 0xD800 && cp <= 0xDFFF)   return false;   // surrogate half
        i += extra + 1;
    }
    return true;
}

} // namespace detail

// ------------------------------------------------------------------ messages

enum class ws_opcode : std::uint8_t {
    continuation = 0x0, text = 0x1, binary = 0x2,
    close = 0x8, ping = 0x9, pong = 0xA
};

struct ws_message {
    bool is_text = true;
    std::string data;
    const std::string& text() const noexcept { return data; }
};

// RFC 6455 §7.4.1 close codes worth naming
enum ws_close : std::uint16_t {
    ws_normal            = 1000,
    ws_going_away        = 1001,
    ws_protocol_error    = 1002,
    ws_unsupported_data  = 1003,
    ws_invalid_payload   = 1007,
    ws_policy_violation  = 1008,
    ws_message_too_big   = 1009,
    ws_internal_error    = 1011
};

// A per-message extension (RFC 6455 §9), negotiated per connection at
// handshake time. This is the same delegate shape as the TLS transport - a
// byte transformer with negotiated state - and for the same reason: it is a
// single-consumer transform with a result, not a fan-out notification. A
// signal would be the wrong tool; the value here is choosing the
// implementation at run time, which is what a delegate is for.
struct ws_config {
    std::size_t max_message = 8u * 1024 * 1024;
    std::size_t max_frame   = 8u * 1024 * 1024;
    // Supply this to offer an extension (see websocket_deflate.hpp). One
    // instance per connection, because the compression context is per
    // connection and stateful.
    std::function<std::unique_ptr<ws_extension>()> extension_factory;
};

namespace detail {

// Shared state between the delegate (owned by the session) and every
// websocket handle the application holds. The handle outliving the connection
// is normal, not an error, so sends on a dead socket are a quiet false.
struct ws_state {
    std::weak_ptr<detail::session_core> session;
    event_loop::poster poster;
    moveable_signal<const ws_message&> on_message;
    moveable_signal<std::uint16_t, const std::string&> on_close;
    moveable_signal<const std::string&> on_ping;
    moveable_signal<const std::string&> on_pong;
    request handshake;
    std::unique_ptr<ws_extension> ext;          // loop thread only
    // Atomic because close() may be called from any thread while the loop
    // thread is closing the same socket from its own side, and connected() is
    // readable from anywhere. A plain bool here is a genuine data race.
    std::atomic<bool> open{true};
    std::vector<scoped_connection> kept;        // connections the handler parked here
};

inline std::string ws_frame(ws_opcode op, const char* data, std::size_t n,
                            bool fin = true, bool rsv1 = false)
{
    std::string f;
    f.reserve(n + 10);
    f.push_back(char((fin ? 0x80 : 0x00) | (rsv1 ? 0x40 : 0x00) |
                     static_cast<std::uint8_t>(op)));
    if (n < 126) {
        f.push_back(char(n));                   // server frames are never masked
    } else if (n <= 0xFFFF) {
        f.push_back(char(126));
        f.push_back(char((n >> 8) & 0xff));
        f.push_back(char(n & 0xff));
    } else {
        f.push_back(char(127));
        for (int i = 7; i >= 0; --i)
            f.push_back(char((std::uint64_t(n) >> (i * 8)) & 0xff));
    }
    f.append(data, n);
    return f;
}

// Split a Sec-WebSocket-Extensions header into its comma-separated offers,
// each "token; param; param=value". Whitespace is insignificant around the
// separators, and quoted values are unwrapped.
inline std::vector<std::pair<std::string, std::string>> parse_extensions(const std::string& v)
{
    std::vector<std::pair<std::string, std::string>> out;
    std::size_t i = 0;
    while (i <= v.size()) {
        std::size_t comma = v.find(',', i);
        if (comma == std::string::npos)
            comma = v.size();
        std::string offer = v.substr(i, comma - i);
        i = comma + 1;

        const std::size_t semi = offer.find(';');
        std::string token = offer.substr(0, semi);
        std::string params = (semi == std::string::npos) ? std::string() : offer.substr(semi + 1);
        auto trim = [](std::string& t) {
            std::size_t b = t.find_first_not_of(" \t");
            std::size_t e = t.find_last_not_of(" \t");
            t = (b == std::string::npos) ? std::string() : t.substr(b, e - b + 1);
        };
        trim(token);
        trim(params);
        if (!token.empty())
            out.emplace_back(std::move(token), std::move(params));
    }
    return out;
}

} // namespace detail

// ------------------------------------------------------------ the handle
//
// Moveable and shareable: park it in a container, hand it to a worker, send
// from any thread. Sends marshal onto the loop thread exactly as responder
// completions do.

class websocket {
public:
    websocket() = default;
    explicit websocket(std::shared_ptr<detail::ws_state> s) : s_(std::move(s)) {}

    // The ergonomic form, and the one to reach for: the socket is handed to
    // the callback rather than captured by it, so the slot holds only a weak
    // reference to the connection state. That matters - a slot capturing a
    // strong handle would own the state that owns the slot, and the socket
    // would never be freed. The connection is parked for you, and dies with
    // the socket, which is the library's own lifetime-tracking idiom applied
    // to itself.
    //
    //     ws.on_message([](auto sock, const auto& m) { sock.send_text(m.data); });
    //
    template <typename F>
    void on_message(F&& f) const
    {
        track(s_->on_message, std::forward<F>(f));
    }
    template <typename F>
    void on_close(F&& f) const { track(s_->on_close, std::forward<F>(f)); }
    template <typename F>
    void on_ping(F&& f) const { track(s_->on_ping, std::forward<F>(f)); }
    template <typename F>
    void on_pong(F&& f) const { track(s_->on_pong, std::forward<F>(f)); }

    // The raw signals, for wiring by hand. Anything connected here is yours to
    // keep alive - see keep() - and must not capture a strong handle.
    moveable_signal<const ws_message&>& on_message() { return s_->on_message; }
    moveable_signal<std::uint16_t, const std::string&>& on_close() { return s_->on_close; }
    moveable_signal<const std::string&>& on_ping() { return s_->on_ping; }
    moveable_signal<const std::string&>& on_pong() { return s_->on_pong; }

    const request& handshake() const noexcept { return s_->handshake; }

    // Bytes queued for this socket but not yet handed to the kernel. The
    // backpressure signal a fan-out needs: a relay with many destinations must
    // be able to tell which of them has fallen behind, and skip it, rather
    // than let one slow peer grow a buffer without bound.
    std::size_t backlog() const noexcept
    {
        auto sess = s_ ? s_->session.lock() : nullptr;
        return sess ? sess->pending_bytes() : 0;
    }
    bool connected() const noexcept
    {
        return s_ && s_->open.load(std::memory_order_acquire) && !s_->session.expired();
    }
    explicit operator bool() const noexcept { return connected(); }

    // A second handle onto the same connection, for capturing into slots
    websocket share() const { return websocket(s_); }

    // Park a connection so it lives as long as the socket does - the usual
    // shape, since the handler returns immediately and the slots must outlive
    // it. Loop thread only, which is where handlers run; the parked list is
    // cleared on the loop thread when the socket closes.
    void keep(scoped_connection c) const { s_->kept.push_back(std::move(c)); }

    bool send_text(std::string payload) const
    {
        return send_frame(ws_opcode::text, std::move(payload));
    }
    bool send_binary(std::string payload) const
    {
        return send_frame(ws_opcode::binary, std::move(payload));
    }
    bool ping(std::string payload = {}) const
    {
        return send_frame(ws_opcode::ping, std::move(payload));
    }
    bool pong(std::string payload = {}) const
    {
        return send_frame(ws_opcode::pong, std::move(payload));
    }

    bool close(std::uint16_t code = ws_normal, const std::string& reason = {}) const
    {
        if (!s_)
            return false;
        std::string body;
        body.push_back(char((code >> 8) & 0xff));
        body.push_back(char(code & 0xff));
        body += reason;
        s_->open.store(false, std::memory_order_release);
        return send_frame(ws_opcode::close, std::move(body));
    }

private:
    // Connect f so that it receives the socket as its first argument and holds
    // only a weak reference to the state, then park the connection so it lives
    // exactly as long as the socket does
    template <typename Signal, typename F>
    void track(Signal& sig, F&& f) const
    {
        std::weak_ptr<detail::ws_state> weak = s_;
        s_->kept.push_back(scoped_connection{
            sig.connect([weak, fn = std::forward<F>(f)](auto&&... args) {
                if (auto st = weak.lock())
                    fn(websocket(st), std::forward<decltype(args)>(args)...);
            })});
    }

    bool send_frame(ws_opcode op, std::string payload) const
    {
        if (!s_)
            return false;
        auto sess = s_->session.lock();
        if (!sess)
            return false;

        // Compression happens inside this, so that it always runs on the loop
        // thread: the deflate context is stateful across messages and must
        // never be touched from two threads
        auto st = s_;
        auto deliver = [st, sess, op, body = std::move(payload)]() mutable {
            bool rsv1 = false;
            const bool control = (static_cast<std::uint8_t>(op) & 0x08) != 0;
            if (!control && st->ext) {
                std::string z;
                if (st->ext->compress(body, z)) {
                    body.swap(z);
                    rsv1 = true;
                }
            }
            const std::string f = detail::ws_frame(op, body.data(), body.size(), true, rsv1);
            sess->push_app(f.data(), f.size());
        };

        if (s_->poster.on_loop_thread()) {
            deliver();
            return true;
        }
        return s_->poster.post(std::move(deliver));
    }

    std::shared_ptr<detail::ws_state> s_;
};

using ws_handler = std::function<void(websocket)>;

// -------------------------------------------------------- protocol delegate

class websocket_protocol final : public protocol_delegate {
public:
    websocket_protocol(std::shared_ptr<detail::ws_state> state, ws_config cfg)
        : st_(std::move(state)), cfg_(cfg) {}

    // Breaking the cycle. The usual handler shape parks a connection in
    // ws_state whose slot captures a websocket handle, which holds ws_state -
    // so the state would outlive the socket forever. The connection dies with
    // the socket, which is both correct and what frees the ring.
    ~websocket_protocol() override
    {
        if (st_) {
            st_->open.store(false, std::memory_order_release);
            st_->kept.clear();
        }
    }

    const char* name() const noexcept override { return "websocket"; }
    bool close_after_flush() const noexcept override { return closed_; }
    bool busy() const noexcept override { return false; }
    bool receiving() const noexcept override { return !buffered_.empty(); }

    // A WebSocket connection is long-lived and idle by design, so the
    // request-shaped response path is never used on it
    void respond(std::uint64_t, response&&, connection_host&) override {}

    bool consume(std::string& in, connection_host& host) override
    {
        std::size_t used = 0;
        while (!closed_) {
            const std::size_t avail = in.size() - used;
            const unsigned char* p = reinterpret_cast<const unsigned char*>(in.data()) + used;
            if (avail < 2)
                break;

            const bool fin = (p[0] & 0x80) != 0;
            const bool rsv1 = (p[0] & 0x40) != 0;
            const bool rsv_other = (p[0] & 0x30) != 0;
            const auto op = static_cast<ws_opcode>(p[0] & 0x0F);
            const bool masked = (p[1] & 0x80) != 0;
            std::uint64_t len = p[1] & 0x7F;
            std::size_t hdr = 2;

            if (rsv_other)                      // RSV2 and RSV3 are never negotiated
                return fail(host, ws_protocol_error, "reserved bits set");
            if (rsv1 && !(st_ && st_->ext))      // RSV1 needs a negotiated extension
                return fail(host, ws_protocol_error, "RSV1 set without an extension");
            if (!masked)                        // RFC 6455 §5.1: mandatory
                return fail(host, ws_protocol_error, "client frame was not masked");

            if (len == 126) {
                if (avail < 4) break;
                len = (std::uint64_t(p[2]) << 8) | p[3];
                hdr = 4;
            } else if (len == 127) {
                if (avail < 10) break;
                len = 0;
                for (int i = 0; i < 8; ++i)
                    len = (len << 8) | p[2 + i];
                hdr = 10;
            }
            const bool control = (static_cast<std::uint8_t>(op) & 0x08) != 0;
            if (control && (len > 125 || !fin))
                return fail(host, ws_protocol_error, "control frame fragmented or oversized");
            if (len > cfg_.max_frame)
                return fail(host, ws_message_too_big, "frame too large");
            if (avail < hdr + 4 + len)
                break;                          // masking key plus payload not here yet

            const unsigned char* key = p + hdr;
            const unsigned char* body = key + 4;
            std::string payload;
            payload.resize(std::size_t(len));
            for (std::size_t i = 0; i < payload.size(); ++i)
                payload[i] = char(body[i] ^ key[i & 3]);
            used += hdr + 4 + std::size_t(len);

            if (rsv1 && control)
                return fail(host, ws_protocol_error, "RSV1 set on a control frame");
            if (!handle_frame(op, fin, control, rsv1, std::move(payload), host))
                break;
        }
        if (used)
            in.erase(0, used);
        return !closed_;
    }

private:
    // RFC 6455 §7.4: only these may appear in a close frame on the wire.
    // 1004 is unassigned; 1005 and 1006 are reserved for local use and must
    // never be sent; 1015 likewise. Everything below 1000, and 1016-2999,
    // is unallocated. A peer sending one of those is a protocol error, not
    // something to echo back politely.
    static bool valid_close_code(std::uint16_t c) noexcept
    {
        if (c >= 3000 && c <= 4999)             // registered and private ranges
            return true;
        switch (c) {
        case 1000: case 1001: case 1002: case 1003:
        case 1007: case 1008: case 1009: case 1010: case 1011:
        case 1012: case 1013: case 1014:
            return true;
        default:
            return false;
        }
    }

    bool fail(connection_host& host, std::uint16_t code, const char* why)
    {
        send_close(host, code, why);
        closed_ = true;
        return false;
    }

    void send_close(connection_host& host, std::uint16_t code, const std::string& reason)
    {
        std::string body;
        body.push_back(char((code >> 8) & 0xff));
        body.push_back(char(code & 0xff));
        body += reason;
        const std::string f = detail::ws_frame(ws_opcode::close, body.data(), body.size());
        host.write_app(f.data(), f.size());
        if (st_ && st_->open.exchange(false, std::memory_order_acq_rel)) {
            // exchange, not load-then-store: only one side announces the close
            st_->on_close(code, reason);
        }
    }

    bool handle_frame(ws_opcode op, bool fin, bool control, bool rsv1, std::string payload,
                      connection_host& host)
    {
        if (control) {
            switch (op) {
            case ws_opcode::close: {
                std::uint16_t code = ws_normal;
                std::string reason;
                if (payload.size() == 1)
                    return fail(host, ws_protocol_error, "malformed close payload");
                if (payload.size() >= 2) {
                    code = std::uint16_t((static_cast<unsigned char>(payload[0]) << 8) |
                                         static_cast<unsigned char>(payload[1]));
                    if (!valid_close_code(code))
                        return fail(host, ws_protocol_error, "invalid close code");
                    reason = payload.substr(2);
                    if (!detail::valid_utf8(reason))
                        return fail(host, ws_invalid_payload, "close reason was not UTF-8");
                }
                send_close(host, code, reason);     // the other half of the handshake
                closed_ = true;
                return false;
            }
            case ws_opcode::ping: {
                const std::string f = detail::ws_frame(ws_opcode::pong, payload.data(), payload.size());
                host.write_app(f.data(), f.size());
                if (st_)
                    st_->on_ping(payload);
                return true;
            }
            case ws_opcode::pong:
                if (st_)
                    st_->on_pong(payload);
                return true;
            default:
                return fail(host, ws_protocol_error, "unknown control opcode");
            }
        }

        if (op == ws_opcode::continuation) {
            if (buffered_.empty() && !fragment_open_)
                return fail(host, ws_protocol_error, "continuation without a start frame");
            if (rsv1)                           // RSV1 marks the message, not each frame
                return fail(host, ws_protocol_error, "RSV1 on a continuation frame");
        } else if (op == ws_opcode::text || op == ws_opcode::binary) {
            if (fragment_open_)
                return fail(host, ws_protocol_error, "new data frame inside a fragment");
            fragment_text_ = (op == ws_opcode::text);
            fragment_compressed_ = rsv1;
            fragment_open_ = true;
        } else {
            return fail(host, ws_protocol_error, "unknown opcode");
        }

        if (buffered_.size() + payload.size() > cfg_.max_message)
            return fail(host, ws_message_too_big, "message too large");
        buffered_ += payload;

        if (!fin)
            return true;

        // Inflate before anything looks at the bytes: a compressed message is
        // not text until it has been decompressed
        if (fragment_compressed_) {
            std::string plain;
            if (!st_ || !st_->ext || !st_->ext->decompress(buffered_, plain))
                return fail(host, ws_invalid_payload, "could not inflate the message");
            if (plain.size() > cfg_.max_message)
                return fail(host, ws_message_too_big, "inflated message too large");
            buffered_.swap(plain);
        }

        // Assembled: validate text as a whole, so sequences split across
        // frames are judged correctly rather than per fragment
        if (fragment_text_ && !detail::valid_utf8(buffered_))
            return fail(host, ws_invalid_payload, "text payload was not UTF-8");

        ws_message m;
        m.is_text = fragment_text_;
        m.data = std::move(buffered_);
        buffered_.clear();
        fragment_open_ = false;
        fragment_compressed_ = false;
        if (st_)
            st_->on_message(m);
        return true;
    }

    std::shared_ptr<detail::ws_state> st_;
    ws_config cfg_;
    std::string buffered_;
    bool fragment_text_ = true;
    bool fragment_compressed_ = false;
    bool fragment_open_ = false;
    bool closed_ = false;
};

// --------------------------------------------------------------- the route
//
// A plain route handler: it validates the Upgrade request, answers 101, and
// swaps the protocol. Nothing in the server had to learn what a WebSocket is.

inline handler websocket_route(ws_handler on_open, ws_config cfg = ws_config{})
{
    return [on_open, cfg](const request& req, responder res) {
        const std::string* upgrade = req.header("upgrade");
        const std::string* conn = req.header("connection");
        const std::string* key = req.header("sec-websocket-key");
        const std::string* ver = req.header("sec-websocket-version");

        if (!upgrade || !detail::iequals(*upgrade, "websocket") || !conn ||
            !detail::has_token(*conn, "upgrade") || !key || key->empty()) {
            res.send(400, "text/plain; charset=utf-8", "400 Bad WebSocket handshake\n");
            return;
        }
        if (!ver || *ver != "13") {
            response r(426);
            r.set("Sec-WebSocket-Version", "13");
            r.content("426 Upgrade Required\n");
            res.send(std::move(r));
            return;
        }

        // RFC 6455 §4.2.2: SHA-1 of the key plus the fixed GUID, base64'd
        const auto digest = detail::sha1(*key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
        response handshake(101);
        handshake.set("Upgrade", "websocket");
        handshake.set("Connection", "Upgrade");
        handshake.set("Sec-WebSocket-Accept", detail::base64(digest.data(), digest.size()));

        auto state = std::make_shared<detail::ws_state>();
        state->handshake = req;
        state->session = res.session();
        state->poster = res.session_poster();

        // Extension negotiation. Each offer is put to the extension in the
        // order the client listed them; the first it accepts wins, and a
        // decline simply means an uncompressed connection rather than a
        // failed handshake.
        if (cfg.extension_factory) {
            if (const std::string* offered = req.header("sec-websocket-extensions")) {
                auto ext = cfg.extension_factory();
                for (const auto& offer : detail::parse_extensions(*offered)) {
                    if (!ext || offer.first != ext->name())
                        continue;
                    std::string response_params;
                    if (ext->negotiate(offer.second, response_params)) {
                        std::string value = ext->name();
                        if (!response_params.empty())
                            value += "; " + response_params;
                        handshake.set("Sec-WebSocket-Extensions", value);
                        state->ext = std::move(ext);
                        break;
                    }
                }
            }
        }
        std::unique_ptr<protocol_delegate> proto(new websocket_protocol(state, cfg));

        res.upgrade(std::move(handshake), std::move(proto));
        on_open(websocket(state));              // slots are wired after the swap is queued
    };
}

} // namespace http
} // namespace snicholls

#endif // SNICHOLLS_HAS_HTTP_SERVER
#endif /* websocket_hpp */
