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
#include "../moveable/atomic.hpp"
#include "../moveable/signal.hpp"

#include <cctype>
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
    std::size_t i = 0;
    if (url.compare(0, 5, "ws://") == 0) {
        i = 5;
        out.secure = false;
        out.port = "80";
    } else if (url.compare(0, 6, "wss://") == 0) {
        i = 6;
        out.secure = true;
        out.port = "443";
    } else {
        return false;
    }
    const std::size_t slash = url.find('/', i);
    std::string authority = url.substr(i, slash == std::string::npos ? std::string::npos : slash - i);
    out.path = (slash == std::string::npos) ? "/" : url.substr(slash);
    if (authority.empty())
        return false;
    // A bracketed IPv6 literal keeps its colons; only the last colon outside
    // brackets separates the port
    if (authority[0] == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string::npos)
            return false;
        out.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size() && authority[close + 1] == ':')
            out.port = authority.substr(close + 2);
    } else {
        const std::size_t colon = authority.rfind(':');
        if (colon == std::string::npos) {
            out.host = authority;
        } else {
            out.host = authority.substr(0, colon);
            out.port = authority.substr(colon + 1);
        }
    }
    return !out.host.empty() && !out.port.empty();
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
    bool auto_reconnect = true;

    // Sent as-is in the handshake: Authorization, Sec-WebSocket-Protocol, and
    // anything else an upstream needs.
    std::vector<std::pair<std::string, std::string>> headers;
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
        if (c_->url.secure)
            return false;               // wss:// needs a TLS transport - see the header note
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
        l->post([c] { c->shutdown(ws_client_status::closed_locally, true); });
    }

    // Queue a text message. Safe from any thread; returns false only when the
    // client is stopped, since a message sent while reconnecting is queued.
    bool send_text(std::string payload)
    {
        auto c = c_;
        event_loop* l = c->loop.load(std::memory_order_relaxed);
        if (!l || c->stopped.load(std::memory_order_relaxed))
            return false;
        l->post([c, p = std::move(payload)]() mutable {
            c->queue_out(ws_opcode::text, std::move(p));
        });
        return true;
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

        int fd = -1;
        event_loop::fd_watch watch;
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

        std::string in;                 // raw bytes from the socket
        std::string out;                // bytes awaiting the socket
        std::size_t sent = 0;
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
            return s;
        }

        std::uint32_t mask_key() { return static_cast<std::uint32_t>(rng()); }

        // ---------------------------------------------------------- connect

        void begin_connect()
        {
            if (stopped.load(std::memory_order_relaxed))
                return;
            addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            addrinfo* res = nullptr;
            if (::getaddrinfo(url.host.c_str(), url.port.c_str(), &hints, &res) != 0 || !res)
                return fail(ws_client_status::connect_failed);

            int s = -1;
            for (addrinfo* a = res; a; a = a->ai_next) {
                s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
                if (s < 0)
                    continue;
                ::fcntl(s, F_SETFL, O_NONBLOCK);
                const int on = 1;
                ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
                const int rc = ::connect(s, a->ai_addr, a->ai_addrlen);
                if (rc == 0 || errno == EINPROGRESS)
                    break;
                ::close(s);
                s = -1;
            }
            ::freeaddrinfo(res);
            if (s < 0)
                return fail(ws_client_status::connect_failed);

            fd = s;
            in.clear();
            out.clear();
            sent = 0;
            frag.clear();
            fragmented = false;

            // Wait for writability: on a non-blocking socket that is how the
            // connect completes, success or refusal alike.
            watch = loop.load(std::memory_order_relaxed)->watch(fd, fd_interest::write);
            auto self = this;
            watch.on_writable([self] { self->on_connected(); });
            watch.on_error([self] { self->fail(ws_client_status::connect_failed); });
        }

        void on_connected()
        {
            int err = 0;
            socklen_t len = sizeof err;
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0)
                return fail(ws_client_status::connect_failed);

            // A 16-byte nonce, base64'd. The accept key we expect back is
            // derived from it now so the reply can be checked rather than
            // trusted.
            unsigned char nonce[16];
            for (int i = 0; i < 16; ++i)
                nonce[i] = static_cast<unsigned char>(rng() & 0xff);
            const std::string key = detail::base64(nonce, sizeof nonce);
            const auto digest = detail::sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
            accept_expected = detail::base64(digest.data(), digest.size());

            std::string req = "GET " + url.path + " HTTP/1.1\r\nHost: " + url.host;
            if (url.port != "80")
                req += ":" + url.port;
            req += "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n";
            req += "Sec-WebSocket-Key: " + key + "\r\n";
            req += "Sec-WebSocket-Version: 13\r\n";
            for (const auto& h : cfg.headers)
                req += h.first + ": " + h.second + "\r\n";
            req += "\r\n";

            out += req;
            handshaking = true;
            watch.set_interest(fd_interest::read_write);
            auto self = this;
            watch.on_readable([self] { self->on_readable(); });
            flush();
        }

        // --------------------------------------------------------- transport

        void flush()
        {
            while (sent < out.size()) {
                const ssize_t n = ::send(fd, out.data() + sent, out.size() - sent, 0);
                if (n > 0) {
                    sent += std::size_t(n);
                    continue;
                }
                if (n < 0 && errno == EINTR)
                    continue;
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    watch.set_interest(fd_interest::read_write);
                    return;
                }
                return fail(ws_client_status::transport_error);
            }
            out.clear();
            sent = 0;
            watch.set_interest(fd_interest::read);
        }

        void on_readable()
        {
            for (;;) {
                char buf[16 * 1024];
                const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
                if (n > 0) {
                    in.append(buf, std::size_t(n));
                    n_bytes_in.fetch_add(std::uint64_t(n), std::memory_order_relaxed);
                    if (std::size_t(n) < sizeof buf)
                        break;
                    continue;
                }
                if (n == 0)
                    return fail(open.load(std::memory_order_relaxed) ? ws_client_status::closed_by_peer
                                     : ws_client_status::handshake_failed);
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return fail(ws_client_status::transport_error);
            }
            if (handshaking && !finish_handshake())
                return;
            if (open.load(std::memory_order_relaxed))
                drain_frames();
        }

        bool finish_handshake()
        {
            const std::size_t end = in.find("\r\n\r\n");
            if (end == std::string::npos) {
                if (in.size() > 64 * 1024) {
                    fail(ws_client_status::handshake_failed);
                    return false;
                }
                return false;               // more to come
            }
            const std::string head = in.substr(0, end);
            in.erase(0, end + 4);

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
                if (in.size() < 2)
                    return;
                const unsigned char* p = reinterpret_cast<const unsigned char*>(in.data());
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
                    if (in.size() < 4) return;
                    len = (std::uint64_t(p[2]) << 8) | p[3];
                    hdr = 4;
                } else if (len == 127) {
                    if (in.size() < 10) return;
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
                if (in.size() < hdr + len)
                    return;                 // wait for the rest

                std::string payload = in.substr(hdr, std::size_t(len));
                in.erase(0, hdr + std::size_t(len));

                if (!handle_frame(op, fin, control, std::move(payload)))
                    return;
                if (!open.load(std::memory_order_relaxed))
                    return;                 // handle_frame tore it down
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

        void queue_out(ws_opcode op, std::string payload)
        {
            if (fd < 0)
                return;                     // reconnecting; the caller's message is dropped
            out += detail::ws_frame_masked(op, payload.data(), payload.size(), mask_key());
            if (op == ws_opcode::text || op == ws_opcode::binary)
                n_messages_out.fetch_add(1, std::memory_order_relaxed);
            if (open.load(std::memory_order_relaxed) || handshaking)
                flush();
        }

        // ------------------------------------------------------- teardown

        void fail(ws_client_status why) { shutdown(why, false); }

        void shutdown(ws_client_status why, bool final_)
        {
            const bool was_open = open.load(std::memory_order_relaxed);
            open.store(false, std::memory_order_relaxed);
            handshaking = false;
            watch.reset();
            if (fd >= 0) {
                ::close(fd);
                fd = -1;
            }
            in.clear();
            out.clear();
            sent = 0;

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
