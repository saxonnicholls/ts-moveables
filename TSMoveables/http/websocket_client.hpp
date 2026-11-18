//
//  websocket_client.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - the outbound half of WebSocket, with survivability
//
//  Everything in this library so far has been the server side: browsers connect
//  to us. This is the other direction - we connect out and stay connected. That
//  is what turns the broadcast hub into a *relay*: N upstream feeds in, one
//  fan-out core, M browsers out, on one reactor and one thread.
//
//      snicholls::event_loop loop;
//      snicholls::http::ws_broadcast_hub hub;
//
//      snicholls::http::websocket_client up;
//      up.on_message([&hub](const ws_message& m) { hub.publish("prices", m.data); });
//      up.connect(loop, "ws://feed.internal:9000/stream");
//
//  The reason this is a component and not ten lines in a demo is the part that
//  is easy to skip: staying connected. A relay that dies when its upstream
//  restarts is not a relay, it is an outage waiting for a deploy. So reconnect
//  is the default and not a feature you remember to add:
//
//    - **Exponential backoff with full jitter.** Delay is random in
//      [0, min(cap, base * 2^attempt)]. The jitter is not decoration - when a
//      feed restarts, every relay that was connected to it reconnects at once,
//      and undithered backoff synchronises them into a thundering herd that
//      knocks the feed straight back over.
//    - **Backoff resets on a *working* session, not on a successful connect.**
//      A server that accepts and immediately drops would otherwise reset the
//      delay every time and spin at full rate. `session_grace` is how long a
//      connection must survive to count as working.
//    - **A close from us is final.** `close()` means stop; only failures
//      reconnect. Otherwise shutdown races the backoff timer forever.
//
//  RFC 6455 obligations that differ from the server side, because this is the
//  end that gets them wrong:
//
//    - **Client frames MUST be masked** (§5.3) with a fresh 32-bit key per
//      frame. Unmasked client frames are a protocol error a real server will
//      close on, so `ws_frame_masked` is the only encoder used here.
//    - **Server frames MUST NOT be masked** (§5.1), and a client receiving a
//      masked frame must fail the connection - so that is checked, not assumed.
//    - The handshake response is *verified*: `Sec-WebSocket-Accept` must equal
//      base64(SHA-1(key + GUID)). Skipping that check means happily talking
//      framed binary at any endpoint that answered 101, which is how a relay
//      ends up silently pointed at the wrong service.
//
//  POSIX only, following the event loop; on Windows this compiles to nothing
//  and SNICHOLLS_HAS_WEBSOCKET_CLIENT is 0.
//

#ifndef ts_moveables_websocket_client_hpp
#define ts_moveables_websocket_client_hpp

#include "websocket.hpp"

#if !SNICHOLLS_HAS_WEBSOCKET
#define SNICHOLLS_HAS_WEBSOCKET_CLIENT 0
#else
#define SNICHOLLS_HAS_WEBSOCKET_CLIENT 1

#include "../event/loop.hpp"
#include "../interfaces/transport_delegate.hpp"
#include "../moveable/atomic.hpp"
#include "../moveable/signal.hpp"
#include "../net/outbound.hpp"
#include "../net/url.hpp"

#include <cctype>
#include <functional>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace snicholls {
namespace http {

namespace detail {

// The client-side encoder. Identical to ws_frame except the mask bit is set and
// the payload is XORed with a per-frame key - RFC 6455 §5.3 requires a fresh,
// unpredictable key on every frame, so the caller passes one in rather than
// this reaching for a global generator.
inline std::string ws_frame_masked(ws_opcode op, const char* data, std::size_t n,
                                   std::uint32_t mask, bool fin = true)
{
    std::string f;
    f.reserve(n + 14);
    f.push_back(char((fin ? 0x80 : 0x00) | static_cast<std::uint8_t>(op)));
    if (n < 126) {
        f.push_back(char(0x80 | n));
    } else if (n <= 0xFFFF) {
        f.push_back(char(0x80 | 126));
        f.push_back(char((n >> 8) & 0xff));
        f.push_back(char(n & 0xff));
    } else {
        f.push_back(char(0x80 | 127));
        for (int i = 7; i >= 0; --i)
            f.push_back(char((static_cast<std::uint64_t>(n) >> (i * 8)) & 0xff));
    }
    unsigned char k[4] = {
        static_cast<unsigned char>((mask >> 24) & 0xff),
        static_cast<unsigned char>((mask >> 16) & 0xff),
        static_cast<unsigned char>((mask >> 8) & 0xff),
        static_cast<unsigned char>(mask & 0xff)};
    f.append(reinterpret_cast<const char*>(k), 4);
    const std::size_t base = f.size();
    f.append(data, n);
    for (std::size_t i = 0; i < n; ++i)
        f[base + i] = char(static_cast<unsigned char>(f[base + i]) ^ k[i & 3]);
    return f;
}

// ws://host:port/path — enough URL parsing for this and no more. Returns false
// on anything it does not understand rather than guessing, because a guess here
// means connecting somewhere the caller did not ask for.
struct ws_url {
    std::string host;
    std::string port = "80";
    std::string path = "/";
    bool secure = false;
};

inline bool parse_ws_url(const std::string& url, ws_url& out)
{
    net::url u;
    if (!net::parse_url(url, u) || (u.scheme != "ws" && u.scheme != "wss"))
        return false;
    out.host = u.host;
    out.port = u.port;
    out.path = u.target;
    out.secure = u.secure;
    return true;
}

} // namespace detail

struct ws_client_config {
    // Reconnect. Delay is random in [0, min(max_backoff, min_backoff << attempt)]
    // - full jitter, so a fleet of relays does not resynchronise on a restart.
    std::chrono::milliseconds min_backoff{100};
    std::chrono::milliseconds max_backoff{30000};

    // How long a connection must last before its attempt counter is forgiven.
    // Without this, a server that accepts then immediately drops resets backoff
    // on every attempt and the client spins at full speed.
    std::chrono::milliseconds session_grace{5000};

    std::size_t max_message = 16u * 1024 * 1024;   // reassembled, across fragments

    // Outbound bound. send_text/send_binary refuse above this and queue
    // NOTHING, rather than accepting everything and growing until the process
    // dies - the failure mode of an unbounded client queue, and one that only
    // shows up once the upstream is slow, which is exactly when you can least
    // afford it. 0 disables the bound, which is the old behaviour said out loud.
    std::size_t max_queue_bytes = 8u * 1024 * 1024;
    bool auto_reconnect = true;

    // Sent as-is in the handshake: Authorization, Sec-WebSocket-Protocol, and
    // anything else an upstream needs.
    std::vector<std::pair<std::string, std::string>> headers;

    // How bytes reach the wire. Empty means plaintext, and `ws://` needs
    // nothing else. `wss://` requires one, and this header deliberately does
    // not know what TLS is - the same two-axis split the server uses, which is
    // what keeps the core dependency-free and the backend a run-time choice:
    //
    //     snicholls::http::openssl_client_context tls;
    //     cfg.transport_factory = [&tls](const std::string& host) {
    //         return tls.connect(host);        // verifies the chain AND the host
    //     };
    //
    // The hostname is passed in because a client transport needs it for two
    // things it cannot get anywhere else: the certificate check and SNI.
    std::function<std::unique_ptr<transport_delegate>(const std::string& host)>
        transport_factory;
};

// Why the connection ended, so a relay can log something useful rather than
// "disconnected".
enum class ws_client_status : std::uint8_t {
    connect_failed,     // TCP never came up
    handshake_failed,   // not a WebSocket endpoint, or the accept key was wrong
    protocol_error,     // the peer broke RFC 6455
    closed_by_peer,     // a clean close frame arrived
    closed_locally,     // close() was called; no reconnect follows
    transport_error     // the socket died mid-session
};

inline const char* to_string(ws_client_status s) noexcept
{
    switch (s) {
    case ws_client_status::connect_failed:   return "connect_failed";
    case ws_client_status::handshake_failed: return "handshake_failed";
    case ws_client_status::protocol_error:   return "protocol_error";
    case ws_client_status::closed_by_peer:   return "closed_by_peer";
    case ws_client_status::closed_locally:   return "closed_locally";
    default:                                 return "transport_error";
    }
}

struct ws_client_stats {
    std::uint64_t connects      = 0;   // successful handshakes
    std::uint64_t reconnects    = 0;   // reconnect attempts started
    std::uint64_t messages_in   = 0;
    std::uint64_t messages_out  = 0;
    std::uint64_t bytes_in      = 0;
    bool          connected     = false;
    std::uint32_t attempt       = 0;   // consecutive failures; 0 when healthy
    // Bytes queued for the socket right now. The server handle has had
    // backlog() from the start; the client had no way to see the same thing.
    std::uint64_t backlog       = 0;
    std::uint64_t rejected      = 0;   // sends refused by max_queue_bytes
};

// ---------------------------------------------------------------- the client
//
// A moveable handle over a shared core, like http::server and ws_broadcast_hub:
// build it configured, move it into place, and the callbacks it parked stay
// wired to the same core.

class websocket_client {
public:
    using config = ws_client_config;
    using status = ws_client_status;
    using stats  = ws_client_stats;

    websocket_client() : websocket_client(config{}) {}
    explicit websocket_client(config cfg) : c_(std::make_shared<core>(std::move(cfg))) {}

    websocket_client(websocket_client&&) noexcept = default;
    websocket_client& operator=(websocket_client&&) noexcept = default;
    websocket_client(const websocket_client&) = delete;
    websocket_client& operator=(const websocket_client&) = delete;

    ~websocket_client() = default;

    // Connect-and-park, the same idiom as everything else here: the callback is
    // held by the core, so it lives exactly as long as the client does.
    template <typename F> void on_message(F&& f) { c_->on_message.connect(std::forward<F>(f)); }
    template <typename F> void on_open(F&& f)    { c_->on_open.connect(std::forward<F>(f)); }
    template <typename F> void on_close(F&& f)   { c_->on_close.connect(std::forward<F>(f)); }

    // Start connecting. Returns false only if the URL is not one we understand -
    // a refused connection is not a failure here, it is the first attempt.
    bool connect(event_loop& loop, const std::string& url)
    {
        if (!detail::parse_ws_url(url, c_->url))
            return false;
        // wss:// needs a transport. Refusing here rather than connecting in
        // plaintext is the point: silently downgrading a wss:// URL would be
        // the worst possible failure - it looks encrypted and is not.
        if (c_->url.secure && !c_->cfg.transport_factory)
            return false;
        c_->loop.store(&loop, std::memory_order_relaxed);
        c_->url_text = url;
        c_->stopped.store(false, std::memory_order_relaxed);
        auto c = c_;
        loop.post([c] { c->begin_connect(); });
        return true;
    }

    // Stop for good. No reconnect follows - a deliberate close is not a failure.
    void close()
    {
        auto c = c_;
        event_loop* l = c->loop.load(std::memory_order_relaxed);
        if (!l)
            return;
        l->post([c] { c->close_gracefully(); });
    }

    // Queue a text message. Safe from any thread; returns false only when the
    // client is stopped, since a message sent while reconnecting is queued.
    //
    // Text means UTF-8 (RFC 6455 5.6) - a peer is entitled to drop the
    // connection over a malformed sequence, which is why arbitrary bytes want
    // send_binary rather than this.
    bool send_text(std::string payload)
    {
        return queue(ws_opcode::text, std::move(payload));
    }

    // Queue a binary message - arbitrary bytes, no encoding contract.
    //
    // The client could always carry binary (queue_out has taken an opcode from
    // the start); only the public door was missing, so callers with bytes had
    // to encode them as text. A downstream consumer was hex-encoding, paying
    // 2x the bytes and the CPU to do it, and moved its object sender to the
    // server side to avoid the asymmetry. Same signature as
    // websocket::send_binary on the server handle.
    bool send_binary(std::string payload)
    {
        return queue(ws_opcode::binary, std::move(payload));
    }

private:
    // One door for both opcodes: the stopped check and the hop onto the loop
    // are the same, and duplicating them is how they drift apart.
    bool queue(ws_opcode op, std::string payload)
    {
        auto c = c_;
        event_loop* l = c->loop.load(std::memory_order_relaxed);
        if (!l || c->stopped.load(std::memory_order_relaxed))
            return false;
        // Bounded, and refusing takes NOTHING - so a producer told false has
        // not half-sent a message and can retry the same bytes once backlog()
        // has fallen. Checked here rather than on the loop, so a caller finds
        // out at the call rather than later in a counter.
        //
        // Reserve-then-check rather than check-then-add, so two threads racing
        // the last of the headroom cannot both get it. And refuse only when
        // something is ALREADY waiting: a single message bigger than the bound,
        // sent onto an empty queue, must still be sendable, or the bound would
        // quietly cap message size as well - the same rule the hub's replay
        // ring follows for a chunk larger than its whole budget.
        const std::size_t size = payload.size();
        const std::size_t cap = c->cfg.max_queue_bytes;
        const std::uint64_t before = c->in_flight.fetch_add(size, std::memory_order_acq_rel);
        if (cap != 0) {
            const std::uint64_t held = before + c->queued_bytes.load(std::memory_order_acquire);
            if (held != 0 && held + size > cap) {
                c->in_flight.fetch_sub(size, std::memory_order_acq_rel);
                c->n_rejected.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
        l->post([c, op, size, p = std::move(payload)]() mutable {
            c->queue_out(op, std::move(p));
            // Released AFTER the append, so for a moment it is counted twice
            // rather than not at all: over-counting is the safe direction for
            // a bound.
            c->in_flight.fetch_sub(size, std::memory_order_acq_rel);
        });
        return true;
    }

public:
    // Bytes queued for the socket. The server handle has had this since the
    // start; the client had no way to see the same thing, so a producer could
    // only find out it was outrunning the upstream by running out of memory.
    std::size_t backlog() const noexcept
    {
        return static_cast<std::size_t>(c_->in_flight.load(std::memory_order_acquire) +
                                        c_->queued_bytes.load(std::memory_order_acquire));
    }

    stats snapshot() const { return c_->snapshot(); }
    bool connected() const noexcept { return c_->open.load(std::memory_order_relaxed); }
    // By value, not by reference: the member is rewritten by connect(), and a
    // reference into it would both race that write and dangle past the
    // client's life. Reflects the most recent connect().
    std::string url() const { return c_->url_text; }

private:
    struct core {
        explicit core(config c) : cfg(std::move(c)), rng(std::random_device{}()) {}

        config cfg;
        // Set by connect() on the caller's thread, read by close() and
        // send_text() on whatever thread calls them. Atomic because "the caller
        // will obviously connect first" is a contract, not a guarantee, and a
        // torn or stale pointer here is a crash rather than a wrong answer.
        moveable_atomic<event_loop*> loop{nullptr};
        detail::ws_url url;
        std::string url_text;

        // The connection: resolution, every address in turn, TLS and the
        // socket all live in net::outbound_connection, shared with
        // http::client. Null while disconnected or waiting to retry.
        std::shared_ptr<net::outbound_connection> link;
        event_loop::timer retry;

        // Written on the loop thread, read from the caller's: connected() and
        // send_text() are public and callable from anywhere, and snapshot() is
        // the whole point of a stats call. TSan caught these as plain bools -
        // the same class of bug as the ws_state::open race Autobahn turned up,
        // which is a reason to fix the pattern and not just this instance.
        //
        // Relaxed throughout, and deliberately: nothing is *published* through
        // these. They carry no payload the reader then dereferences, so the
        // ordering they need is none - only that the read is not torn and the
        // compiler cannot hoist it out of a spin.
        moveable_atomic<bool> stopped{false};
        moveable_atomic<bool> open{false};       // handshake complete
        moveable_atomic_uint32_t attempt{0};

        bool handshaking = false;       // loop thread only
        std::chrono::steady_clock::time_point session_start{};

        // Consumed with an offset, never erase(0, n) per frame. Erasing the
        // front of the buffer memmoves everything behind it, so draining a
        // window full of small frames costs O(bytes x frames) - quadratic in
        // the batch. Same idiom as h2_stream::out_buf and the hub's queues.
        std::string in;                 // raw bytes from the socket
        std::size_t in_off = 0;         // how much of `in` is already consumed
        // Published so send() can bound itself from ANY thread without taking
        // the loop. Written on the loop, read anywhere.
        std::atomic<std::uint64_t> queued_bytes{0};   // appended, not yet on the wire
        // Accepted by send() but still in a posted task the loop has not run.
        // Counted separately because the loop only knows what it has APPENDED:
        // bound against that alone and a fast producer can post far past the
        // cap before the loop catches up, which makes it a suggestion rather
        // than a bound.
        std::atomic<std::uint64_t> in_flight{0};
        std::atomic<std::uint64_t> n_rejected{0};
        std::string accept_expected;

        // Fragment reassembly, per RFC 6455 §5.4
        std::string frag;
        ws_opcode frag_op = ws_opcode::text;
        bool fragmented = false;

        std::mt19937 rng;
        moveable_signal<const ws_message&> on_message;
        moveable_signal<> on_open;
        moveable_signal<ws_client_status> on_close;

        moveable_atomic_uint64_t n_connects{0};
        moveable_atomic_uint64_t n_reconnects{0};
        moveable_atomic_uint64_t n_messages_in{0};
        moveable_atomic_uint64_t n_messages_out{0};
        moveable_atomic_uint64_t n_bytes_in{0};

        ws_client_stats snapshot() const
        {
            ws_client_stats s;
            s.connects     = n_connects.load(std::memory_order_relaxed);
            s.reconnects   = n_reconnects.load(std::memory_order_relaxed);
            s.messages_in  = n_messages_in.load(std::memory_order_relaxed);
            s.messages_out = n_messages_out.load(std::memory_order_relaxed);
            s.bytes_in     = n_bytes_in.load(std::memory_order_relaxed);
            s.connected    = open.load(std::memory_order_relaxed);
            s.attempt      = attempt.load(std::memory_order_relaxed);
            s.backlog      = in_flight.load(std::memory_order_acquire) +
                             queued_bytes.load(std::memory_order_acquire);
            s.rejected     = n_rejected.load(std::memory_order_relaxed);
            return s;
        }

        std::uint32_t mask_key() { return static_cast<std::uint32_t>(rng()); }

        // ---------------------------------------------------------- connect

        void begin_connect()
        {
            if (stopped.load(std::memory_order_relaxed))
                return;
            in.clear();
            in_off = 0;
            frag.clear();
            fragmented = false;
            event_loop* l = loop.load(std::memory_order_relaxed);
            // No connect timeout: as before, the operating system's decides, and the backoff takes it from there
            link = std::make_shared<net::outbound_connection>(*l, url.host, url.port, cfg.transport_factory,
                                                              std::chrono::milliseconds{0});
            auto self = this;
            link->on_open = [self] { self->send_handshake(); };
            link->on_data = [self](std::string& data) { self->on_data(data); };
            link->on_close = [self](net::outbound_status why) { self->on_link_closed(why); };
            link->start();
        }

        void on_link_closed(net::outbound_status why)
        {
            switch (why) {
            case net::outbound_status::closed_by_us:
                return;                     // we closed it: shutdown() is already running
            case net::outbound_status::closed_by_peer:
                return fail(open.load(std::memory_order_relaxed) ? ws_client_status::closed_by_peer
                                                                 : ws_client_status::handshake_failed);
            case net::outbound_status::handshake_failed:
                return fail(ws_client_status::handshake_failed);
            case net::outbound_status::transport_error:
                return fail(open.load(std::memory_order_relaxed) ? ws_client_status::transport_error
                                                                 : ws_client_status::handshake_failed);
            default:
                return fail(ws_client_status::connect_failed);
            }
        }

        void send_handshake()
        {
            // A 16-byte nonce, base64'd. The accept key we expect back is
            // derived from it now so the reply can be checked rather than
            // trusted.
            unsigned char nonce[16];
            for (int i = 0; i < 16; ++i)
                nonce[i] = static_cast<unsigned char>(rng() & 0xff);
            const std::string key = detail::base64(nonce, sizeof nonce);
            const auto digest = detail::sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
            accept_expected = detail::base64(digest.data(), digest.size());

            const bool v6 = url.host.find(':') != std::string::npos;
            std::string req = "GET " + url.path + " HTTP/1.1\r\nHost: " + (v6 ? "[" + url.host + "]" : url.host);
            if (url.port != (url.secure ? "443" : "80"))
                req += ":" + url.port;
            req += "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n";
            req += "Sec-WebSocket-Key: " + key + "\r\n";
            req += "Sec-WebSocket-Version: 13\r\n";
            for (const auto& h : cfg.headers)
                req += h.first + ": " + h.second + "\r\n";
            req += "\r\n";
            handshaking = true;
            if (!write_app(req.data(), req.size()))
                return fail(ws_client_status::transport_error);
        }

        // --------------------------------------------------------- transport

        // The one door out: plaintext to the connection, which encrypts and
        // sends it. Nothing else in this class writes to the wire.
        bool write_app(const char* data, std::size_t n) { return link && link->write(data, n); }

        void on_data(std::string& data)
        {
            n_bytes_in.fetch_add(std::uint64_t(data.size()), std::memory_order_relaxed);
            in.append(data);
            data.clear();
            if (handshaking && !finish_handshake())
                return;
            if (open.load(std::memory_order_relaxed))
                drain_frames();
        }

        bool finish_handshake()
        {
            const std::size_t end = in.find("\r\n\r\n");   // handshake: offset is still 0
            if (end == std::string::npos) {
                if (in.size() > 64 * 1024) {
                    fail(ws_client_status::handshake_failed);
                    return false;
                }
                return false;               // more to come
            }
            const std::string head = in.substr(0, end);
            in.erase(0, end + 4);
            in_off = 0;

            if (head.compare(0, 12, "HTTP/1.1 101") != 0) {
                fail(ws_client_status::handshake_failed);
                return false;
            }
            // Verify the accept key. Anything can answer 101; only the endpoint
            // that saw our nonce can produce this.
            std::string lower = head;
            for (char& ch : lower)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            const std::size_t at = lower.find("sec-websocket-accept:");
            if (at == std::string::npos) {
                fail(ws_client_status::handshake_failed);
                return false;
            }
            std::size_t vs = head.find(':', at) + 1;
            while (vs < head.size() && (head[vs] == ' ' || head[vs] == '\t'))
                ++vs;
            std::size_t ve = head.find('\r', vs);
            if (ve == std::string::npos)
                ve = head.size();
            if (head.substr(vs, ve - vs) != accept_expected) {
                fail(ws_client_status::handshake_failed);
                return false;
            }

            handshaking = false;
            open.store(true, std::memory_order_relaxed);
            n_connects.fetch_add(1, std::memory_order_relaxed);
            // `attempt` is deliberately NOT cleared here. Connecting is not the
            // same as working: a server that accepts and drops would reset the
            // backoff on every attempt and turn reconnect into a hot loop. It
            // is cleared in shutdown(), once a session has outlived
            // session_grace and thereby earned it.
            session_start = std::chrono::steady_clock::now();
            on_open();
            return true;
        }

        // ------------------------------------------------------------ frames

        void drain_frames()
        {
            for (;;) {
                const std::size_t avail = in.size() - in_off;
                if (avail < 2)
                    return compact_in();
                const unsigned char* p =
                    reinterpret_cast<const unsigned char*>(in.data()) + in_off;
                const bool fin = (p[0] & 0x80) != 0;
                const bool rsv = (p[0] & 0x70) != 0;
                const ws_opcode op = static_cast<ws_opcode>(p[0] & 0x0f);
                const bool masked = (p[1] & 0x80) != 0;
                std::uint64_t len = p[1] & 0x7f;
                std::size_t hdr = 2;

                // RFC 6455 §5.1: a server must never mask. Extensions are not
                // negotiated here, so a reserved bit is equally a protocol error.
                if (masked || rsv)
                    return fail(ws_client_status::protocol_error);

                if (len == 126) {
                    if (avail < 4) return compact_in();
                    len = (std::uint64_t(p[2]) << 8) | p[3];
                    hdr = 4;
                } else if (len == 127) {
                    if (avail < 10) return compact_in();
                    len = 0;
                    for (int i = 0; i < 8; ++i)
                        len = (len << 8) | p[2 + i];
                    hdr = 10;
                }
                const bool control = (static_cast<std::uint8_t>(op) & 0x08) != 0;
                if (control && (len > 125 || !fin))
                    return fail(ws_client_status::protocol_error);
                if (len > cfg.max_message)
                    return fail(ws_client_status::protocol_error);
                if (avail < hdr + len)
                    return compact_in();    // wait for the rest

                std::string payload(in, in_off + hdr, std::size_t(len));
                in_off += hdr + std::size_t(len);

                if (!handle_frame(op, fin, control, std::move(payload)))
                    return;
                if (!open.load(std::memory_order_relaxed))
                    return;                 // handle_frame tore it down
            }
        }

        // Reclaim the consumed prefix once it is worth the single memmove -
        // amortised O(1) per byte instead of one memmove per frame.
        void compact_in()
        {
            if (in_off && in_off * 2 >= in.size()) {
                in.erase(0, in_off);
                in_off = 0;
            }
        }

        bool handle_frame(ws_opcode op, bool fin, bool control, std::string payload)
        {
            if (control) {
                if (op == ws_opcode::ping) {
                    queue_out(ws_opcode::pong, std::move(payload));
                } else if (op == ws_opcode::close) {
                    // Echo the close, then go. A peer close is not a failure,
                    // but it does reconnect - the upstream may just be cycling.
                    queue_out(ws_opcode::close, std::string());
                    shutdown(ws_client_status::closed_by_peer, false);
                    return false;
                }
                return true;                // pong: nothing to do
            }

            if (op == ws_opcode::continuation) {
                if (!fragmented) {
                    fail(ws_client_status::protocol_error);
                    return false;
                }
                frag += payload;
            } else {
                if (fragmented) {           // a new data frame mid-fragment
                    fail(ws_client_status::protocol_error);
                    return false;
                }
                frag = std::move(payload);
                frag_op = op;
                fragmented = !fin;
            }
            if (frag.size() > cfg.max_message) {
                fail(ws_client_status::protocol_error);
                return false;
            }
            if (!fin)
                return true;                // more fragments coming

            fragmented = false;
            ws_message m;
            m.is_text = (frag_op == ws_opcode::text);
            m.data = std::move(frag);
            frag.clear();
            n_messages_in.fetch_add(1, std::memory_order_relaxed);
            on_message(m);
            return true;
        }

        void publish_backlog()
        {
            queued_bytes.store(link ? link->backlog() : 0, std::memory_order_release);
        }

        void queue_out(ws_opcode op, std::string payload)
        {
            if (!link)
                return;                     // reconnecting; the caller's message is dropped
            const std::string f =
                detail::ws_frame_masked(op, payload.data(), payload.size(), mask_key());
            if (!write_app(f.data(), f.size()))
                return fail(ws_client_status::transport_error);
            if (op == ws_opcode::text || op == ws_opcode::binary)
                n_messages_out.fetch_add(1, std::memory_order_relaxed);
            publish_backlog();
        }

        // ------------------------------------------------------- teardown

        // RFC 6455 7.1.2: the endpoint that initiates a close sends a Close
        // frame before the socket goes. Without one the peer reads a bare EOF
        // and reports 1006 - abnormal closure - so every DELIBERATE close was
        // indistinguishable from a crash, and a server that treats the two
        // differently (reconnect policy, alerting, unsubscribing) learned
        // nothing from a clean shutdown.
        //
        // Written directly rather than through queue_out, which calls fail() on
        // a write error: that would run shutdown() once here and once below,
        // firing on_close twice for a single close. A frame we could not write
        // is simply skipped - the socket is going anyway.
        //
        // SHUT_WR after the frame so the FIN follows it rather than racing it:
        // the peer reads Close, then EOF, in that order.
        void close_gracefully()
        {
            if (link && open.load(std::memory_order_relaxed)) {
                std::string body;
                body.push_back(char((ws_normal >> 8) & 0xff));
                body.push_back(char(ws_normal & 0xff));
                const std::string f = detail::ws_frame_masked(
                    ws_opcode::close, body.data(), body.size(), mask_key());
                if (write_app(f.data(), f.size()) && link)
                    link->shutdown_write();
            }
            shutdown(ws_client_status::closed_locally, true);
        }

        void fail(ws_client_status why) { shutdown(why, false); }

        // Close the connection without hearing about it, and let it go. It may be
        // the one calling us (its on_close), so it is released after this task
        void drop_link()
        {
            if (!link)
                return;
            auto l = std::move(link);
            l->on_open = nullptr;
            l->on_data = nullptr;
            l->on_close = nullptr;
            l->close();
            if (event_loop* lp = loop.load(std::memory_order_relaxed))
                lp->post([l] {});
        }

        void shutdown(ws_client_status why, bool final_)
        {
            const bool was_open = open.load(std::memory_order_relaxed);
            open.store(false, std::memory_order_relaxed);
            handshaking = false;
            drop_link();
            in.clear();
            in_off = 0;
            queued_bytes.store(0, std::memory_order_release);

            if (final_)
                stopped.store(true, std::memory_order_relaxed);

            // A session that lasted counts as working, so the next failure
            // starts its backoff from scratch rather than from wherever the
            // last outage left the counter.
            if (was_open &&
                std::chrono::steady_clock::now() - session_start >= cfg.session_grace)
                attempt.store(0, std::memory_order_relaxed);
            else if (was_open || why != ws_client_status::closed_locally)
                attempt.fetch_add(1, std::memory_order_relaxed);

            on_close(why);

            if (!stopped.load(std::memory_order_relaxed) && cfg.auto_reconnect)
                schedule_retry();
        }

        void schedule_retry()
        {
            // Full jitter: uniform in [0, ceiling]. Undithered backoff makes a
            // fleet reconnect in lockstep and re-break whatever just recovered.
            std::uint64_t ceiling = std::uint64_t(cfg.min_backoff.count());
            const std::uint32_t a_ = attempt.load(std::memory_order_relaxed);
            const std::uint32_t shift = a_ < 20 ? a_ : 20;
            ceiling <<= shift;
            const std::uint64_t cap = std::uint64_t(cfg.max_backoff.count());
            if (ceiling > cap || ceiling == 0)
                ceiling = cap;
            std::uniform_int_distribution<std::uint64_t> pick(0, ceiling);
            const auto delay = std::chrono::milliseconds(pick(rng));

            n_reconnects.fetch_add(1, std::memory_order_relaxed);
            retry = loop.load(std::memory_order_relaxed)->after(delay);
            auto self = this;
            retry.on_fire([self] { self->begin_connect(); });
        }
    };

    std::shared_ptr<core> c_;
};

} // namespace http
} // namespace snicholls

#endif /* SNICHOLLS_HAS_WEBSOCKET */
#endif /* ts_moveables_websocket_client_hpp */
