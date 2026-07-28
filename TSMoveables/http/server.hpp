//
//  http_server.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - a non-blocking HTTP server on the event loop
//
//  Phase 1 of FUTURE_DIRECTIONS §8: plaintext HTTP/1.1, built as a reactor on
//  snicholls::event_loop, with every policy expressed as a runtime delegate
//  rather than a compile-time #ifdef. The two axes the design hangs on:
//
//    transport_delegate  - how bytes reach us  (plain now; TLS, QUIC later)
//    protocol_delegate   - how bytes become requests  (http/1.1 now; h2, h3)
//
//  Both are chosen per connection at run time, which is what lets one binary
//  serve http/1.1 and h2 by ALPN later without a rebuild. Interfaces carry a
//  stream id from the start (always 0 here) because HTTP/3 rides QUIC streams
//  and retrofitting that is a rewrite.
//
//  Handlers receive a moveable, complete-once `responder`: answer inline, or
//  move it onto a thread pool and answer later from any thread - completions
//  marshal back through the loop. A blocking server cannot do that, which is
//  why its ceiling is its thread count. Dropping a responder unanswered sends
//  500 rather than hanging the client: loud failure over undefined behaviour.
//
//      snicholls::http::server srv;
//      srv.get("/hello/:name", [](const auto& req, auto res) {
//          res.send(200, "text/plain", "hello " + req.param("name"));
//      });
//      srv.listen("0.0.0.0", 8080);
//      srv.run();
//
//  The parser is deliberately strict. Content-Length with Transfer-Encoding,
//  conflicting duplicate Content-Length, whitespace before a colon, obsolete
//  line folding and bare-LF line endings are all rejected - every one is a
//  documented request-smuggling vector, and leniency is how a server becomes
//  a gadget in someone else's attack chain.
//
//  POSIX only in phase 1 (it follows event_loop); on Windows this header
//  compiles to nothing and SNICHOLLS_HAS_HTTP_SERVER is 0.
//

#ifndef http_server_hpp
#define http_server_hpp

#include "../event/loop.hpp"
#include "../interfaces/transport_delegate.hpp"
#include "../moveable/signal.hpp"

#if !SNICHOLLS_HAS_EVENT_LOOP
#define SNICHOLLS_HAS_HTTP_SERVER 0
#else
#define SNICHOLLS_HAS_HTTP_SERVER 1

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef NI_MAXHOST
#define NI_MAXHOST 1025
#endif
#ifndef NI_MAXSERV
#define NI_MAXSERV 32
#endif

#include "message.hpp"
#include "parser.hpp"
#include "config.hpp"
#include "../interfaces/protocol_delegate.hpp"

namespace snicholls {
namespace http {

// ------------------------------------------------------------------ HTTP/1.1

class http1_protocol final : public protocol_delegate {
public:
    explicit http1_protocol(const parse_limits& lim) : parser_(lim) {}

    const char* name() const noexcept override { return "http/1.1"; }
    bool close_after_flush() const noexcept override { return close_; }
    bool busy() const noexcept override { return in_flight_; }
    bool receiving() const noexcept override { return parser_.started(); }
    std::size_t last_response_bytes() const noexcept override { return last_bytes_; }

    bool consume(std::string& in, connection_host& host) override
    {
        // One request in flight at a time: HTTP/1.1 responses must come back
        // in request order, so pipelining is serialised rather than raced
        while (!in.empty() && !in_flight_ && !close_ && !handed_off_) {
            std::size_t used = 0;
            const auto st = parser_.parse(in.data(), in.size(), used);
            if (used)
                in.erase(0, used);

            if (st == request_parser::status::failed) {
                close_ = true;
                host.protocol_failure(parser_.error_status(), parser_.error_reason());
                return false;
            }
            if (st == request_parser::status::need_more) {
                if (parser_.headers_done() && parser_.expects_continue() && !sent_continue_) {
                    sent_continue_ = true;
                    static const char k[] = "HTTP/1.1 100 Continue\r\n\r\n";
                    host.write_app(k, sizeof k - 1);
                }
                return true;
            }

            request& req = parser_.message();
            head_ = (req.method == method::head);
            keep_alive_ = req.keep_alive;
            req_minor_ = req.http_minor;
            request_bytes_ = req.body.size();
            sent_continue_ = false;
            in_flight_ = true;
            // Handed over by reference and reset afterwards: handlers already
            // see a const& valid only for the call, so nothing needs to own it
            host.deliver(req, 0);
            const bool alive = host.live();
            parser_.reset();
            if (!alive)
                return false;
        }
        return true;
    }

    void respond(std::uint64_t, response&& res, connection_host& host) override
    {
        std::string out;
        serialise(res, head_, keep_alive_, host, out);
        host.write_app(out.data(), out.size());
        last_bytes_ = out.size();
        in_flight_ = false;
        if (res.status == 101)
            handed_off_ = true;                 // another protocol owns the bytes now
        else if (!keep_alive_)
            close_ = true;
    }

    std::size_t last_request_bytes() const noexcept { return request_bytes_; }

    bool begin_stream(std::uint64_t, response&& res, connection_host& host) override
    {
        // A caller who set Content-Length knows the size and wants it framed
        // that way; everyone else gets chunked, which is what makes an
        // unknown-length body possible at all on HTTP/1.1
        chunked_ = !res.headers.has("content-length");
        if (chunked_ && req_minor_ < 1) {
            // HTTP/1.0 has no chunked coding. The honest framing is to close
            // the connection at the end of the body and say so.
            chunked_ = false;
            close_on_end_ = true;
            keep_alive_ = false;
        }
        if (chunked_)
            res.set("Transfer-Encoding", "chunked");

        std::string out;
        serialise_head(res, keep_alive_ && !close_on_end_, host, out);
        host.write_app(out.data(), out.size());
        last_bytes_ = out.size();
        streaming_ = true;                      // in_flight_ stays true until end_stream
        return true;
    }

    void stream_write(std::uint64_t, const char* data, std::size_t n,
                      connection_host& host) override
    {
        if (!streaming_ || n == 0 || head_)     // a HEAD response has no body
            return;
        if (chunked_) {
            char hdr[24];
            const int k = std::snprintf(hdr, sizeof hdr, "%zx\r\n", n);
            host.write_app(hdr, std::size_t(k));
            host.write_app(data, n);
            host.write_app("\r\n", 2);
            last_bytes_ += std::size_t(k) + n + 2;
        } else {
            host.write_app(data, n);
            last_bytes_ += n;
        }
    }

    void end_stream(std::uint64_t, connection_host& host) override
    {
        if (!streaming_)
            return;
        if (chunked_ && !head_) {
            host.write_app("0\r\n\r\n", 5);   // terminal chunk, no trailers
            last_bytes_ += 5;
        }
        streaming_ = false;
        in_flight_ = false;
        if (!keep_alive_ || close_on_end_)
            close_ = true;
    }

    static void serialise(const response& res, bool head_only, bool keep_alive,
                          connection_host& host, std::string& out)
    {
        out.reserve(192 + res.body.size());
        serialise_prologue(res, keep_alive, host, out, /*with_length*/ true);
        if (!head_only)
            out += res.body;
    }

    // Status line and headers only - what a streamed response sends up front
    static void serialise_head(const response& res, bool keep_alive,
                               connection_host& host, std::string& out)
    {
        serialise_prologue(res, keep_alive, host, out, /*with_length*/ false);
    }

private:
    // with_length adds Content-Length from the body size, which a streamed
    // response must not do: its length is not known yet, which is the entire
    // reason it is streamed.
    static void serialise_prologue(const response& res, bool keep_alive,
                                   connection_host& host, std::string& out,
                                   bool with_length)
    {
        if (const char* line = status_line(res.status)) {
            out += line;                        // the common codes, pre-baked
        } else {
            out += "HTTP/1.1 ";
            detail::append_uint(out, static_cast<unsigned long long>(res.status));
            out += ' ';
            out += status_text(res.status);
            out += "\r\n";
        }
        if (!res.headers.has("date")) {
            out += "Date: ";
            out += host.http_date();
            out += "\r\n";
        }
        if (!res.headers.has("server")) {
            out += "Server: ";
            detail::append_sanitised(out, host.config().server_name);
            out += "\r\n";
        }
        const bool switching = (res.status == 101);
        if (with_length && !switching && !res.headers.has("content-length")) {
            out += "Content-Length: ";
            detail::append_uint(out, res.body.size());
            out += "\r\n";
        }
        if (!switching && !res.headers.has("connection"))
            out += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
        for (const auto& h : res.headers) {
            detail::append_sanitised(out, h.first);
            out += ": ";
            detail::append_sanitised(out, h.second);
            out += "\r\n";
        }
        out += "\r\n";
    }

private:
    request_parser parser_;
    std::size_t last_bytes_ = 0;
    std::size_t request_bytes_ = 0;
    bool in_flight_ = false;
    bool head_ = false;
    bool keep_alive_ = true;
    bool close_ = false;
    bool sent_continue_ = false;
    bool handed_off_ = false;
    bool streaming_ = false;                    // a response body is still being written
    bool chunked_ = false;                      // that body is chunk-framed
    bool close_on_end_ = false;                 // HTTP/1.0 streaming: length is the close
    int req_minor_ = 1;
};

// ------------------------------------------------------------------- routing

namespace detail {
struct session_core;
struct server_core;
} // namespace detail

class response_stream;

// A moveable, complete-once handle to an unanswered request. Answer it now,
// or move it anywhere - a queue, a thread pool, another loop - and answer it
// later from any thread. Destroying one unanswered sends 500 rather than
// leaving the client hanging.
class responder {
public:
    responder() = default;
    responder(const responder&) = delete;
    responder& operator=(const responder&) = delete;

    responder(responder&& o) noexcept
        : s_(std::move(o.s_)), stream_(o.stream_), answered_(o.answered_)
    {
        o.answered_ = true;
    }

    responder& operator=(responder&& o) noexcept
    {
        if (this != &o) {
            abandon();
            s_ = std::move(o.s_);
            stream_ = o.stream_;
            answered_ = o.answered_;
            o.answered_ = true;
        }
        return *this;
    }

    ~responder() { abandon(); }

    void send(response res);
    void send(int status, std::string content_type, std::string body);
    void send_status(int status);

    // Answer with a body written in pieces rather than all at once. The
    // headers go out now; the body follows through the returned handle.
    response_stream stream(response headers);

    // Answer this request by handing the connection to another protocol: the
    // handshake response goes out, then `next` owns every byte that follows.
    // This is the Upgrade path (WebSocket today, h2c later) and it is why
    // protocol is a delegate rather than a hard-wired parser.
    void upgrade(response handshake, std::unique_ptr<protocol_delegate> next);

    bool answered() const noexcept { return answered_; }
    std::uint64_t stream() const noexcept { return stream_; }

    // For a protocol that outlives this request (the Upgrade path): a weak
    // reference to the connection, and the handle for getting work back onto
    // its loop thread. Weak on purpose - the client may vanish first, and a
    // send on a dead connection should be a quiet false, not a crash.
    std::weak_ptr<detail::session_core> session() const noexcept { return s_; }
    event_loop::poster session_poster() const;
    bool connected() const noexcept { return !s_.expired(); }
    explicit operator bool() const noexcept { return !answered_ && connected(); }

private:
    friend struct detail::session_core;
    void abandon();

    std::weak_ptr<detail::session_core> s_;
    std::uint64_t stream_ = 0;
    bool answered_ = true;              // a default-constructed responder is inert
};

// A response whose body is written in pieces, because its length is not known
// when the headers go out - a file, a query result, a Server-Sent Events feed.
// Moveable and completable from any thread, exactly like responder: park it on
// a worker and feed it as the data arrives.
//
//     srv.get("/big", [](const auto&, auto res) {
//         auto out = res.stream(snicholls::http::response(200).type("text/plain"));
//         for (auto& chunk : source) out.write(chunk);
//         out.end();
//     });
//
// Backpressure is the point of pending() and on_drain(): a producer faster
// than the client will otherwise queue the whole thing in memory, which is the
// problem streaming exists to solve. Check pending() against a budget, and
// resume from on_drain().
class response_stream {
public:
    response_stream() = default;
    response_stream(const response_stream&) = delete;
    response_stream& operator=(const response_stream&) = delete;

    response_stream(response_stream&& o) noexcept
        : s_(std::move(o.s_)), stream_(o.stream_), open_(o.open_)
    {
        o.open_ = false;
    }
    response_stream& operator=(response_stream&& o) noexcept
    {
        if (this != &o) {
            end();
            s_ = std::move(o.s_);
            stream_ = o.stream_;
            open_ = o.open_;
            o.open_ = false;
        }
        return *this;
    }

    // Dropping a stream ends it cleanly rather than leaving the client waiting
    // on a chunked body that never terminates
    ~response_stream() { end(); }

    bool write(const char* data, std::size_t n);
    bool write(std::string chunk) { return write(chunk.data(), chunk.size()); }
    void end();

    bool open() const noexcept { return open_ && !s_.expired(); }
    explicit operator bool() const noexcept { return open(); }

    // Bytes queued for this connection and not yet gone: the socket backlog
    // plus anything the protocol is holding because flow control will not let
    // it out yet. On HTTP/2 the second part is usually the larger one, and a
    // producer that only saw the first would think a stalled peer was idle.
    // Safe from any thread; the value is published by the loop, so it may lag
    // by one operation.
    std::size_t pending() const;
    // Fires on the loop thread when the queue empties
    moveable_signal<>* on_drain() const;

private:
    friend class responder;
    std::weak_ptr<detail::session_core> s_;
    std::uint64_t stream_ = 0;
    bool open_ = false;
};

using handler = std::function<void(const request&, responder)>;

class router {
public:
    void add(method m, const std::string& pattern, handler h)
    {
        route r;
        r.m = m;
        r.h = std::move(h);
        std::size_t i = 0;
        while (i < pattern.size()) {
            if (pattern[i] == '/') { ++i; continue; }
            std::size_t j = pattern.find('/', i);
            if (j == std::string::npos)
                j = pattern.size();
            r.segments.emplace_back(pattern, i, j - i);
            i = j;
        }
        if (!r.segments.empty() && r.segments.back() == "*") {
            r.wildcard = true;
            r.segments.pop_back();
        }
        routes_.push_back(std::move(r));
    }

    // Returns the handler, or null. path_exists reports "the path matched but
    // the method did not", which is a 405 rather than a 404.
    //
    // Deliberately allocation-free until a route actually wins. The earlier
    // shape built a fresh hash map for every route it *tried*, so an eight
    // route table cost eight map constructions per request; segments are now
    // matched as views over the path, and captures collected only once.
    const handler* match(const request& req,
                         std::unordered_map<std::string, std::string>& params,
                         bool& path_exists, std::string& allowed) const
    {
        path_exists = false;

        const std::size_t n = count_segments(req.path);
        seg inline_segs[kInlineSegments];
        std::vector<seg> heap_segs;                 // only for pathological paths
        seg* segs = inline_segs;
        if (n > kInlineSegments) {
            heap_segs.resize(n);
            segs = heap_segs.data();
        }
        split(req.path, segs);

        const route* winner = nullptr;
        for (const auto& r : routes_) {
            if (!shape_matches(r, segs, n))
                continue;
            path_exists = true;
            if (r.m == req.method) {
                winner = &r;
                break;
            }
            if (!allowed.empty())
                allowed += ", ";
            allowed += to_string(r.m);
        }
        if (!winner)
            return nullptr;

        // Only the winning route pays for captures
        for (std::size_t i = 0; i < winner->segments.size(); ++i) {
            const std::string& pat = winner->segments[i];
            if (!pat.empty() && pat[0] == ':')
                params.emplace(pat.substr(1), std::string(segs[i].p, segs[i].n));
        }
        if (winner->wildcard) {
            std::string rest;
            for (std::size_t i = winner->segments.size(); i < n; ++i) {
                rest += '/';
                rest.append(segs[i].p, segs[i].n);
            }
            params.emplace("*", std::move(rest));
        }
        return &winner->h;
    }

    std::size_t size() const noexcept { return routes_.size(); }

private:
    struct route {
        method m = method::get;
        std::vector<std::string> segments;
        bool wildcard = false;
        handler h;
    };

    // A path segment as a view over the request's own path: no copy, no
    // allocation, nothing to free
    struct seg {
        const char* p = nullptr;
        std::size_t n = 0;
    };
    static const std::size_t kInlineSegments = 32;

    static std::size_t count_segments(const std::string& path) noexcept
    {
        std::size_t i = 0, k = 0;
        while (i < path.size()) {
            if (path[i] == '/') { ++i; continue; }
            const std::size_t j = path.find('/', i);
            ++k;
            i = (j == std::string::npos) ? path.size() : j;
        }
        return k;
    }

    static void split(const std::string& path, seg* out) noexcept
    {
        std::size_t i = 0, k = 0;
        while (i < path.size()) {
            if (path[i] == '/') { ++i; continue; }
            std::size_t j = path.find('/', i);
            if (j == std::string::npos)
                j = path.size();
            out[k].p = path.data() + i;
            out[k].n = j - i;
            ++k;
            i = j;
        }
    }

    static bool shape_matches(const route& r, const seg* segs, std::size_t n) noexcept
    {
        if (r.wildcard ? n < r.segments.size() : n != r.segments.size())
            return false;
        for (std::size_t i = 0; i < r.segments.size(); ++i) {
            const std::string& pat = r.segments[i];
            if (!pat.empty() && pat[0] == ':')
                continue;                       // a capture matches any segment
            if (pat.size() != segs[i].n || std::memcmp(pat.data(), segs[i].p, segs[i].n) != 0)
                return false;
        }
        return true;
    }

    std::vector<route> routes_;
};

// -------------------------------------------------------------- the internals

namespace detail {

inline bool set_nonblocking(int fd) noexcept
{
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

inline void suppress_sigpipe(int fd) noexcept
{
#ifdef SO_NOSIGPIPE
    const int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#else
    (void)fd;
#endif
}

inline int send_flags() noexcept
{
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

struct server_core;

struct session_core final : connection_host, std::enable_shared_from_this<session_core> {
    std::weak_ptr<server_core> srv;
    int fd = -1;
    std::uint64_t id = 0;
    connection_info info;

    event_loop::fd_watch watch;
    event_loop::poster poster;
    scoped_connection c_read, c_write, c_error;

    std::unique_ptr<transport_delegate> transport;
    std::unique_ptr<protocol_delegate> protocol;
    std::unique_ptr<protocol_delegate> pending_protocol;

    std::string app_in;                 // plaintext awaiting the parser
    std::string out;                    // wire bytes awaiting the socket
    std::size_t sent = 0;

    bool live_ = true;
    bool want_write = false;
    bool read_paused = false;
    bool close_when_drained = false;
    bool driving = false;
    bool peer_closed = false;
    bool logging_ = false;                  // is anything attached to on_access?
    moveable_signal<> on_drain;             // the write queue emptied: see response_stream

    std::chrono::steady_clock::time_point last_activity{};
    std::chrono::steady_clock::time_point request_started{};
    access_entry pending{};

    ~session_core() override
    {
        watch.reset();
        if (fd >= 0)
            ::close(fd);
    }

    // ---- connection_host
    const server_config& config() const noexcept override;
    const char* http_date() override;
    bool live() const noexcept override { return live_; }
    void deliver(request& req, std::uint64_t stream) override;
    void protocol_failure(int status, const char* reason) override;

    const char* alpn() const noexcept override { return transport ? transport->alpn() : ""; }

    void write_app(const char* data, std::size_t n) override
    {
        if (!transport->app_out(data, n, out))
            close_politely(false);              // the transport queued its alert
    }

    void switch_protocol(std::unique_ptr<protocol_delegate> next) override
    {
        pending_protocol = std::move(next);
    }

    // Send application bytes outside a request/response exchange - what a
    // protocol needs once it owns the connection (a WebSocket frame, say).
    // Loop thread only; the public handles marshal for their callers.
    void push_app(const char* data, std::size_t n)
    {
        if (!live_)
            return;
        write_app(data, n);
        flush();
    }

    // What a streaming producer must throttle against. The socket queue is
    // only half of it: a multiplexed protocol can be holding bytes that flow
    // control will not let it emit yet, and those are just as much a backlog.
    //
    // Loop thread only - it reads `out`, `sent` and the delegate's stream
    // table, all of which the loop owns. Producers read `pending_pub` instead.
    std::size_t pending_bytes() const noexcept
    {
        return (out.size() - sent) + (protocol ? protocol->buffered_bytes() : 0);
    }

    // The published copy, and the reason it exists.
    //
    // A streaming producer runs on a worker thread and asks pending() whether
    // to keep going. Answering that from the producer's own thread means
    // reading a std::string's size while the loop appends to it, and - once a
    // multiplexed protocol is involved - walking a std::map while the loop
    // inserts into it. So the loop computes the number at the points where it
    // can change and publishes it here, and the producer reads only this.
    // Slightly stale by construction, which is the correct trade: backpressure
    // is advisory, and the hard bound that actually protects memory is
    // enforced inside the protocol on the loop thread.
    std::atomic<std::size_t> pending_pub{0};

    void publish_pending() noexcept
    {
        pending_pub.store(pending_bytes(), std::memory_order_relaxed);
    }

    // Close, but let the transport have its say first.
    //
    // A TLS transport queues bytes that explain the ending: an alert on a
    // fatal error (handshake_failure, bad_certificate), a close_notify on a
    // clean shutdown. Closing the descriptor without writing them leaves the
    // peer with a bare FIN and no reason - which is exactly the sort of thing
    // that gets diagnosed as a network fault for a week. One best-effort
    // non-blocking write: we are closing regardless, so this takes whatever
    // the socket will accept now rather than waiting for room.
    //
    // say_goodbye is false after a fatal transport error, because the
    // transport has already queued its alert and asking a broken session to
    // shut down cleanly is not meaningful.
    void close_politely(bool say_goodbye)
    {
        if (live_ && fd >= 0) {
            if (say_goodbye && transport)
                transport->shutdown(out);
            if (sent < out.size()) {
                const ssize_t n = ::send(fd, out.data() + sent, out.size() - sent, send_flags());
                if (n > 0)
                    sent += std::size_t(n);
            }
        }
        close_now();
    }

    // The streaming trio, all loop-thread only; the handles marshal for their
    // callers. Writes coalesce inside the read loop for the same reason
    // complete() does - one write() per batch rather than per piece.
    bool begin_stream(std::uint64_t id, response&& headers)
    {
        if (!live_)
            return false;
        if (!protocol->begin_stream(id, std::move(headers), *this))
            return false;
        if (!driving)
            flush();
        return true;
    }

    void stream_write(std::uint64_t id, const char* data, std::size_t n)
    {
        if (!live_)
            return;
        protocol->stream_write(id, data, n, *this);
        if (!driving)
            flush();                            // which republishes
        else
            publish_pending();                  // coalescing; nothing else will
    }

    void end_stream(std::uint64_t id);

    // ---- reactor plumbing
    void on_readable();
    void on_writable() { flush(); }
    void drive();
    void flush();
    void close_now();
    void complete(std::uint64_t stream, response&& res);
    void arm_write(bool on);
    void pause_read(bool on);
    void send_timeout();
};

struct server_core : std::enable_shared_from_this<server_core> {
    event_loop loop;
    server_config cfg;
    router routes;
    handler fallback;

    std::unordered_map<int, std::shared_ptr<session_core>> sessions;
    int listen_fd = -1;
    std::uint16_t bound_port = 0;
    event_loop::fd_watch listen_watch;
    scoped_connection listen_conn;
    event_loop::timer sweeper;
    scoped_connection sweep_conn;
    int reserve_fd = -1;

    std::vector<char> readbuf;
    std::string date_cache;
    std::time_t date_second = 0;

    std::uint64_t next_id = 1;
    std::uint64_t total_connections = 0;
    std::uint64_t total_requests = 0;

    moveable_signal<const connection_info&> on_open;
    moveable_signal<const connection_info&> on_close;
    moveable_signal<const access_entry&> on_access;
    moveable_signal<const char*, int> on_error;
    std::vector<scoped_connection> kept;         // connect-and-park; see below

    std::function<std::unique_ptr<transport_delegate>()> make_transport =
        [] { return std::unique_ptr<transport_delegate>(new plain_transport()); };

    // The other half of the matrix. A connection's protocol is a run-time
    // choice too, and it has to be one: over TLS the ALPN result does not
    // exist yet when the descriptor is accepted, so the delegate installed
    // here is free to be one that decides later and swaps itself out.
    std::function<std::unique_ptr<protocol_delegate>(const server_config&)> make_protocol =
        [](const server_config& sc) {
            return std::unique_ptr<protocol_delegate>(new http1_protocol(sc.limits));
        };

    explicit server_core(server_config c) : cfg(std::move(c))
    {
        readbuf.resize(cfg.read_buffer ? cfg.read_buffer : 64u * 1024);
        // The EMFILE reserve: one descriptor held back so that running out of
        // them can be survived rather than spun on (see shed_connection)
        reserve_fd = ::open("/dev/null", O_RDONLY);
    }

    ~server_core()
    {
        shutdown();
        if (reserve_fd >= 0)
            ::close(reserve_fd);
    }

    void shutdown()
    {
        sweep_conn.disconnect();
        sweeper.cancel();
        listen_conn.disconnect();
        listen_watch.reset();
        if (listen_fd >= 0) {
            ::close(listen_fd);
            listen_fd = -1;
        }
        auto doomed = std::move(sessions);
        sessions.clear();
        for (auto& kv : doomed)
            kv.second->live_ = false;
    }

    const char* http_date()
    {
        const std::time_t now = std::time(nullptr);
        if (now != date_second || date_cache.empty()) {
            date_second = now;
            std::tm tm{};
#if defined(_WIN32)
            ::gmtime_s(&tm, &now);
#else
            ::gmtime_r(&now, &tm);
#endif
            // Formatted by hand: strftime's %a and %b are locale-dependent,
            // and an HTTP date must be C-locale English whatever the host is
            static const char* const days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
            static const char* const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
            char buf[40];
            std::snprintf(buf, sizeof buf, "%s, %02d %s %04d %02d:%02d:%02d GMT",
                          days[tm.tm_wday % 7], tm.tm_mday, months[tm.tm_mon % 12],
                          tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
            date_cache = buf;
        }
        return date_cache.c_str();
    }

    void accept_ready()
    {
        for (int i = 0; i < cfg.accept_burst; ++i) {
            sockaddr_storage ss{};
            socklen_t sl = sizeof ss;
            const int cfd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&ss), &sl);
            if (cfd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return;
                if (errno == EINTR || errno == ECONNABORTED)
                    continue;
                if (errno == EMFILE || errno == ENFILE) {
                    shed_connection();
                    return;
                }
                on_error("accept failed", errno);
                return;
            }
            if (cfg.max_connections && sessions.size() >= cfg.max_connections) {
                ::close(cfd);
                continue;
            }
            adopt(cfd, ss);
        }
    }

    // The classic reactor trap: with no descriptors left, accept() fails
    // forever and a level-triggered loop spins at 100% CPU serving nobody.
    // Give up the reserve, use it to accept-and-close one pending connection
    // (the client gets a reset instead of a hang), then take the reserve back.
    void shed_connection()
    {
        if (reserve_fd >= 0) {
            ::close(reserve_fd);
            reserve_fd = -1;
        }
        const int cfd = ::accept(listen_fd, nullptr, nullptr);
        if (cfd >= 0)
            ::close(cfd);
        reserve_fd = ::open("/dev/null", O_RDONLY);
        on_error("out of file descriptors: connection shed", EMFILE);
    }

    void adopt(int cfd, const sockaddr_storage& ss)
    {
        if (!set_nonblocking(cfd)) {
            ::close(cfd);
            on_error("could not set O_NONBLOCK", errno);
            return;
        }
        suppress_sigpipe(cfd);
        if (cfg.tcp_nodelay) {
            const int on = 1;
            ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
        }

        auto s = std::make_shared<session_core>();
        s->srv = weak_from_this();
        s->fd = cfd;
        s->id = next_id++;
        s->transport = make_transport();
        s->protocol = make_protocol(cfg);
        if (!s->protocol)
            s->protocol.reset(new http1_protocol(cfg.limits));
        s->poster = loop.make_poster();
        s->last_activity = s->request_started = std::chrono::steady_clock::now();
        s->info = describe(cfd, ss);

        try {
            s->watch = loop.watch(cfd, fd_interest::read);
        } catch (const std::exception&) {
            on_error("could not watch connection", errno);
            return;                             // session_core's destructor closes the fd
        }
        std::weak_ptr<session_core> weak = s;
        s->c_read = s->watch.on_readable().connect([weak] {
            if (auto sp = weak.lock())
                sp->on_readable();
        });
        s->c_write = s->watch.on_writable().connect([weak] {
            if (auto sp = weak.lock())
                sp->on_writable();
        });
        s->c_error = s->watch.on_error().connect([weak] {
            if (auto sp = weak.lock())
                sp->close_now();
        });

        sessions.emplace(cfd, s);
        ++total_connections;
        on_open(s->info);
    }

    static connection_info describe(int fd, const sockaddr_storage& ss)
    {
        connection_info ci;
        ci.fd = fd;
        char host[NI_MAXHOST] = {0}, serv[NI_MAXSERV] = {0};
        socklen_t len = (ss.ss_family == AF_INET6) ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
        if (::getnameinfo(reinterpret_cast<const sockaddr*>(&ss), len, host, sizeof host,
                          serv, sizeof serv, NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
            ci.peer = host;
            ci.peer_port = static_cast<std::uint16_t>(std::atoi(serv));
        }
        return ci;
    }

    void sweep()
    {
        const auto now = std::chrono::steady_clock::now();
        std::vector<std::shared_ptr<session_core>> slow, idle;
        for (auto& kv : sessions) {
            session_core& s = *kv.second;
            if (s.protocol->busy())
                continue;                       // an async handler is still working
            if (s.protocol->receiving()) {
                if (now - s.request_started > cfg.request_timeout)
                    slow.push_back(kv.second);
            } else if (now - s.last_activity > cfg.idle_timeout) {
                idle.push_back(kv.second);
            }
        }
        for (auto& s : slow)
            s->send_timeout();
        for (auto& s : idle)
            s->close_politely(true);            // an idle timeout is still a clean close
    }
};

// ---- session_core methods that need server_core complete

inline const server_config& session_core::config() const noexcept
{
    static const server_config fallback_cfg{};
    auto s = srv.lock();
    return s ? s->cfg : fallback_cfg;
}

inline const char* session_core::http_date()
{
    auto s = srv.lock();
    return s ? s->http_date() : "";
}

inline void session_core::arm_write(bool on)
{
    if (on == want_write || !live_)
        return;
    want_write = on;
    const fd_interest want = (read_paused ? fd_interest::none : fd_interest::read) |
                             (on ? fd_interest::write : fd_interest::none);
    watch.set_interest(want);
}

inline void session_core::pause_read(bool on)
{
    if (on == read_paused || !live_)
        return;
    read_paused = on;
    const fd_interest want = (on ? fd_interest::none : fd_interest::read) |
                             (want_write ? fd_interest::write : fd_interest::none);
    watch.set_interest(want);
}

inline void session_core::on_readable()
{
    auto self = shared_from_this();             // survive a handler that closes us
    auto s = srv.lock();
    if (!s) {
        close_now();
        return;
    }
    const server_config& cfg = s->cfg;

    for (int i = 0; i < cfg.reads_per_event && live_; ++i) {
        const ssize_t n = ::recv(fd, s->readbuf.data(), s->readbuf.size(), 0);
        if (n > 0) {
            last_activity = std::chrono::steady_clock::now();
            if (!protocol->receiving())
                request_started = last_activity;
            if (!transport->wire_in(s->readbuf.data(), std::size_t(n), app_in, out)) {
                close_politely(false);          // the transport queued its alert
                return;
            }
            drive();
            if (!live_)
                return;
            if (out.size() - sent >= cfg.write_high_water) {
                pause_read(true);
                break;
            }
            if (std::size_t(n) < s->readbuf.size())
                break;                          // socket drained
        } else if (n == 0) {
            peer_closed = true;
            close_when_drained = true;
            break;
        } else {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            close_now();
            return;
        }
    }
    flush();
}

inline void session_core::drive()
{
    // Reentrancy guard: an inline response re-enters here from complete(),
    // while the outer consume() loop is still on the stack
    if (driving || !live_)
        return;
    driving = true;
    for (;;) {
        protocol->consume(app_in, *this);
        if (!pending_protocol || !live_)
            break;
        // The Upgrade completed: the old delegate has returned, so replacing
        // it is safe, and the new one gets whatever bytes are already buffered
        protocol = std::move(pending_protocol);
    }
    driving = false;
}

inline void session_core::flush()
{
    if (!live_)
        return;
    while (sent < out.size()) {
        const ssize_t n = ::send(fd, out.data() + sent, out.size() - sent, send_flags());
        if (n > 0) {
            sent += std::size_t(n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            arm_write(true);
            publish_pending();                  // the backlogged case: publish it
            return;
        }
        close_now();
        return;
    }

    const bool had_backlog = !out.empty();
    out.clear();
    sent = 0;
    if (out.capacity() > 256u * 1024)
        std::string().swap(out);
    arm_write(false);
    // The socket queue is empty, but a multiplexed protocol may still be
    // holding bytes its send window will not take - so republish rather than
    // assume zero.
    publish_pending();
    // Everything queued has reached the kernel. A streaming producer waiting
    // on backpressure can send more now.
    if (had_backlog)
        on_drain();
    if (read_paused)
        pause_read(false);
    if (close_when_drained || protocol->close_after_flush())
        close_politely(true);                   // clean end: let TLS say close_notify
}

inline void session_core::close_now()
{
    if (!live_)
        return;
    live_ = false;
    auto self = weak_from_this().lock();        // erase below may drop the last reference
    if (auto s = srv.lock()) {
        s->on_close(info);
        s->sessions.erase(fd);
    }
    c_read.disconnect();
    c_write.disconnect();
    c_error.disconnect();
    watch.reset();
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

inline void session_core::send_timeout()
{
    static const char k[] =
        "HTTP/1.1 408 Request Timeout\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
    auto self = shared_from_this();
    write_app(k, sizeof k - 1);
    close_when_drained = true;
    flush();
}

inline void session_core::protocol_failure(int status, const char* reason)
{
    response res(status ? status : 400);
    res.set("Connection", "close");
    res.content(std::string(status_text(res.status)) + "\n");
    if (reason && *reason)
        res.set("X-Reason", reason);

    std::string out_bytes;
    http1_protocol::serialise(res, false, false, *this, out_bytes);
    write_app(out_bytes.data(), out_bytes.size());
    close_when_drained = true;

    if (auto s = srv.lock()) {
        access_entry e;
        e.method = pending.method;
        e.path = pending.path;
        e.status = res.status;
        e.response_bytes = out_bytes.size();
        e.fd = fd;
        s->on_access(e);
    }
}

inline void session_core::deliver(request& req, std::uint64_t stream)
{
    auto s = srv.lock();
    if (!s) {
        close_now();
        return;
    }
    ++s->total_requests;

    // Only pay for the access-log snapshot when something is listening. These
    // are three string copies per request, and most servers run with no tap
    // attached at all.
    pending.status = 0;
    pending.response_bytes = 0;
    pending.duration_ms = 0.0;
    pending.fd = fd;
    pending.stream = stream;
    pending.request_bytes = req.body.size();
    logging_ = (s->on_access.slot_count() != 0);
    if (logging_) {
        pending.method = req.method_text;
        pending.path = req.path;
        pending.query = req.query;
    }
    request_started = std::chrono::steady_clock::now();

    responder r;
    r.s_ = weak_from_this();
    r.stream_ = stream;
    r.answered_ = false;

    std::unordered_map<std::string, std::string> params;
    bool path_exists = false;
    std::string allowed;
    const handler* h = s->routes.match(req, params, path_exists, allowed);

    if (h) {
        req.params = std::move(params);
        (*h)(req, std::move(r));
    } else if (path_exists) {
        response res(405);
        res.set("Allow", allowed);
        res.content("405 Method Not Allowed\n");
        r.send(std::move(res));
    } else if (s->fallback) {
        s->fallback(req, std::move(r));
    } else {
        r.send(404, "text/plain; charset=utf-8", "404 Not Found\n");
    }
}

inline void session_core::end_stream(std::uint64_t id)
{
    if (!live_)
        return;
    auto self = shared_from_this();
    protocol->end_stream(id, *this);
    if (logging_) {
        if (auto s = srv.lock()) {
            pending.response_bytes = protocol->last_response_bytes();
            pending.duration_ms = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - request_started).count();
            s->on_access(pending);
        }
    }
    if (!driving) {
        flush();
        if (live_)
            drive();                            // a pipelined request may be waiting
        if (live_)
            flush();
    }
}

inline void session_core::complete(std::uint64_t stream, response&& res)
{
    if (!live_)
        return;
    auto self = shared_from_this();
    const int status = res.status;
    protocol->respond(stream, std::move(res), *this);

    if (logging_) {
        if (auto s = srv.lock()) {
            pending.status = status;
            pending.response_bytes = protocol->last_response_bytes();
            pending.duration_ms = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - request_started).count();
            s->on_access(pending);
        }
    }

    // Coalesce. While we are still inside the read loop (driving), more
    // pipelined requests may follow, and on_readable() flushes once at the
    // end - so a batch of N pipelined requests costs one write() instead of
    // N. Answering off-thread lands here with driving false and must go out
    // immediately, since no read loop is coming back to do it.
    if (!driving) {
        flush();
        if (live_)
            drive();                            // a pipelined request may be waiting
        if (live_)
            flush();
    }
}

} // namespace detail

// ---- responder methods, now that session_core is complete

inline void responder::send(response res)
{
    if (answered_)
        throw std::logic_error("http::responder: already answered");
    answered_ = true;
    auto s = s_.lock();
    if (!s)
        return;                                 // the client went away: nothing to do

    if (s->poster.on_loop_thread()) {
        s->complete(stream_, std::move(res));
        return;
    }
    const std::uint64_t stream = stream_;
    auto keep = s;
    if (!s->poster.post([keep, stream, r = std::move(res)]() mutable {
            keep->complete(stream, std::move(r));
        })) {
        // The loop is gone: the connection cannot be answered, and saying so
        // quietly beats crashing on the way down
    }
}

inline event_loop::poster responder::session_poster() const
{
    auto s = s_.lock();
    return s ? s->poster : event_loop::poster{};
}

inline void responder::upgrade(response res, std::unique_ptr<protocol_delegate> next)
{
    if (answered_)
        throw std::logic_error("http::responder: already answered");
    if (!next)
        throw std::invalid_argument("http::responder: upgrade needs a protocol");
    answered_ = true;
    auto s = s_.lock();
    if (!s)
        return;

    if (s->poster.on_loop_thread()) {
        s->switch_protocol(std::move(next));
        s->complete(stream_, std::move(res));
        return;
    }
    // Upgrading from a worker thread (after an async authentication, say).
    // A unique_ptr cannot be captured into a std::function, so it travels
    // inside a shared_ptr and is moved back out on the loop thread.
    auto carrier = std::make_shared<std::unique_ptr<protocol_delegate>>(std::move(next));
    auto keep = s;
    const std::uint64_t stream = stream_;
    keep->poster.post([keep, stream, carrier, r = std::move(res)]() mutable {
        keep->switch_protocol(std::move(*carrier));
        keep->complete(stream, std::move(r));
    });
}

inline response_stream responder::stream(response headers)
{
    response_stream out;
    if (answered_)
        throw std::logic_error("http::responder: already answered");
    answered_ = true;
    auto s = s_.lock();
    if (!s)
        return out;                             // client gone: an inert stream
    if (!s->poster.on_loop_thread())
        throw std::logic_error("http::responder: stream() must start on the loop thread "
                               "(answer with send() from a worker, or post() first)");
    if (!s->begin_stream(stream_, std::move(headers)))
        return out;                             // the protocol cannot stream this exchange
    out.s_ = s_;
    out.stream_ = stream_;
    out.open_ = true;
    return out;
}

inline bool response_stream::write(const char* data, std::size_t n)
{
    if (!open_ || n == 0)
        return open_;
    auto s = s_.lock();
    if (!s)
        return false;
    if (s->poster.on_loop_thread()) {
        s->stream_write(stream_, data, n);
        return true;
    }
    // From a worker: the bytes must be copied, since the caller's buffer is
    // not ours to hold on to across the hop
    auto keep = s;
    const std::uint64_t id = stream_;
    return keep->poster.post([keep, id, chunk = std::string(data, n)] {
        keep->stream_write(id, chunk.data(), chunk.size());
    });
}

inline void response_stream::end()
{
    if (!open_)
        return;
    open_ = false;
    auto s = s_.lock();
    if (!s)
        return;
    if (s->poster.on_loop_thread()) {
        s->end_stream(stream_);
        return;
    }
    auto keep = s;
    const std::uint64_t id = stream_;
    keep->poster.post([keep, id] { keep->end_stream(id); });
}

inline std::size_t response_stream::pending() const
{
    // The published value, never the live one: this is nearly always called
    // from the producer's thread, and the live one belongs to the loop.
    auto s = s_.lock();
    return s ? s->pending_pub.load(std::memory_order_relaxed) : 0;
}

inline moveable_signal<>* response_stream::on_drain() const
{
    auto s = s_.lock();
    return s ? &s->on_drain : nullptr;
}

inline void responder::send(int status, std::string content_type, std::string body)
{
    response res(status);
    res.content(std::move(body), std::move(content_type));
    send(std::move(res));
}

inline void responder::send_status(int status)
{
    response res(status);
    res.content(std::string(status_text(status)) + "\n");
    send(std::move(res));
}

inline void responder::abandon()
{
    if (answered_)
        return;
    answered_ = true;
    auto s = s_.lock();
    if (!s)
        return;
    // A handler that drops its responder without answering is a bug. Say so
    // with a 500 rather than leaving the client waiting for a timeout.
    response res(500);
    res.content("500 Internal Server Error\n");
    const std::uint64_t stream = stream_;
    if (s->poster.on_loop_thread()) {
        s->complete(stream, std::move(res));
        return;
    }
    auto keep = s;
    s->poster.post([keep, stream, r = std::move(res)]() mutable {
        keep->complete(stream, std::move(r));
    });
}

// ---------------------------------------------------------------- the server
//
// Moveable, like everything else here: the core is heap-stable, so a fully
// configured server is a value you can build in a factory and move into place.

class server {
public:
    explicit server(server_config cfg = server_config{})
        : c_(std::make_shared<detail::server_core>(std::move(cfg))) {}

    server(server&&) noexcept = default;
    server& operator=(server&&) noexcept = default;
    server(const server&) = delete;
    server& operator=(const server&) = delete;

    // ---- routing
    server& route(method m, const std::string& pattern, handler h)
    {
        c_->routes.add(m, pattern, std::move(h));
        return *this;
    }
    server& get(const std::string& p, handler h)     { return route(method::get, p, std::move(h)); }
    server& post(const std::string& p, handler h)    { return route(method::post, p, std::move(h)); }
    server& put(const std::string& p, handler h)     { return route(method::put, p, std::move(h)); }
    server& del(const std::string& p, handler h)     { return route(method::del, p, std::move(h)); }
    server& patch(const std::string& p, handler h)   { return route(method::patch, p, std::move(h)); }
    server& head(const std::string& p, handler h)    { return route(method::head, p, std::move(h)); }
    server& options(const std::string& p, handler h) { return route(method::options, p, std::move(h)); }

    server& not_found(handler h)
    {
        c_->fallback = std::move(h);
        return *this;
    }

    // The transport factory - where a TLS delegate is installed at run time
    server& transport_factory(std::function<std::unique_ptr<transport_delegate>()> f)
    {
        c_->make_transport = std::move(f);
        return *this;
    }

    // The protocol factory - where an h2-aware delegate is installed at run
    // time. See http2.hpp's enable_http2(), which is this in one line.
    server& protocol_factory(
        std::function<std::unique_ptr<protocol_delegate>(const server_config&)> f)
    {
        c_->make_protocol = std::move(f);
        return *this;
    }

    // ---- lifecycle
    // Binds, listens and starts accepting. Port 0 binds an ephemeral port;
    // the bound port is returned (and available from port()).
    std::uint16_t listen(const std::string& host = "0.0.0.0", std::uint16_t port = 8080)
    {
        auto& c = *c_;
        if (c.listen_fd >= 0)
            throw std::logic_error("http::server: already listening");

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
        char port_text[8];
        std::snprintf(port_text, sizeof port_text, "%u", unsigned(port));

        addrinfo* list = nullptr;
        const int rc = ::getaddrinfo(host.empty() ? nullptr : host.c_str(), port_text, &hints, &list);
        if (rc != 0)
            throw std::runtime_error(std::string("http::server: getaddrinfo: ") + ::gai_strerror(rc));

        int fd = -1;
        for (addrinfo* a = list; a && fd < 0; a = a->ai_next) {
            fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (fd < 0)
                continue;
            const int on = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
            if (c.cfg.reuse_port) {
                // SO_REUSEPORT_LB (FreeBSD) actually load-balances; plain
                // SO_REUSEPORT does so on Linux but not on macOS/BSD, where it
                // only permits the duplicate bind. See listen_shared().
#ifdef SO_REUSEPORT_LB
                ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT_LB, &on, sizeof on);
#elif defined(SO_REUSEPORT)
                ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof on);
#endif
            }
            if (a->ai_family == AF_INET6) {
                const int v6only = 0;
                ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof v6only);
            }
            if (::bind(fd, a->ai_addr, a->ai_addrlen) != 0 || ::listen(fd, c.cfg.backlog) != 0) {
                ::close(fd);
                fd = -1;
            }
        }
        ::freeaddrinfo(list);
        if (fd < 0)
            throw std::runtime_error("http::server: could not bind " + host + ":" + port_text);

        detail::set_nonblocking(fd);
        detail::suppress_sigpipe(fd);
        c.listen_fd = fd;
        c.bound_port = actual_port(fd);
        arm_listener();
        return c.bound_port;
    }

    // Accept from a listening socket someone else already bound - the portable
    // way to run several reactors, each with its own loop and thread, on one
    // port. The descriptor is duplicated so this server owns its own and can
    // be shut down independently.
    //
    // Why this exists: SO_REUSEPORT load-balances incoming connections on
    // Linux, but on macOS and the BSDs it only permits the duplicate bind -
    // delivery still goes to a single socket, so N reactors silently become 1.
    // (FreeBSD's balancing flag is SO_REUSEPORT_LB, used automatically below
    // where it exists.) Sharing one listener works everywhere: every loop
    // watches it and whichever wakes first wins the non-blocking accept.
    std::uint16_t listen_shared(int listening_fd)
    {
        auto& c = *c_;
        if (c.listen_fd >= 0)
            throw std::logic_error("http::server: already listening");
        if (listening_fd < 0)
            throw std::invalid_argument("http::server: bad listening descriptor");

        const int fd = ::dup(listening_fd);
        if (fd < 0)
            throw std::runtime_error("http::server: could not duplicate the listening socket");
        detail::set_nonblocking(fd);
        detail::suppress_sigpipe(fd);
        c.listen_fd = fd;
        c.bound_port = actual_port(fd);
        arm_listener();
        return c.bound_port;
    }

    // The listening descriptor, for handing to listen_shared() on sibling
    // reactors. Owned by this server; do not close it.
    int listener() const noexcept { return c_->listen_fd; }

    void run() { c_->loop.run(); }
    bool run_once(std::chrono::milliseconds max_wait = std::chrono::milliseconds{0})
    {
        return c_->loop.run_once(max_wait);
    }
    void stop() { c_->loop.stop(); }
    bool running() const noexcept { return c_->loop.running(); }

    // Close the listener and every live connection, without destroying the server
    void shutdown() { c_->shutdown(); }

    event_loop& loop() noexcept { return c_->loop; }
    std::uint16_t port() const noexcept { return c_->bound_port; }
    std::size_t connections() const noexcept { return c_->sessions.size(); }
    std::uint64_t total_connections() const noexcept { return c_->total_connections; }
    std::uint64_t total_requests() const noexcept { return c_->total_requests; }
    const server_config& config() const noexcept { return c_->cfg; }

    // ---- taps: logging, metrics and tracing, off the hot path
    //
    // Connect and park: the connection lives as long as the server, so there
    // is no scoped_connection to hold on to. Forgetting to hold one is the
    // classic way to wire a log handler that silently never fires.
    //
    //     srv.on_access([](const auto& e) { log(e.method, e.path, e.status); });
    //
    template <typename F> server& on_open(F&& f)   { return park(c_->on_open, std::forward<F>(f)); }
    template <typename F> server& on_close(F&& f)  { return park(c_->on_close, std::forward<F>(f)); }
    template <typename F> server& on_access(F&& f) { return park(c_->on_access, std::forward<F>(f)); }
    template <typename F> server& on_error(F&& f)  { return park(c_->on_error, std::forward<F>(f)); }

    // The raw signals, for wiring by hand and owning the connection
    moveable_signal<const connection_info&>& on_open() noexcept { return c_->on_open; }
    moveable_signal<const connection_info&>& on_close() noexcept { return c_->on_close; }
    moveable_signal<const access_entry&>& on_access() noexcept { return c_->on_access; }
    moveable_signal<const char*, int>& on_error() noexcept { return c_->on_error; }

private:
    template <typename Sig, typename F>
    server& park(Sig& sig, F&& f)
    {
        c_->kept.push_back(scoped_connection{sig.connect(std::forward<F>(f))});
        return *this;
    }

    void arm_listener()
    {
        auto& c = *c_;
        auto weak = std::weak_ptr<detail::server_core>(c_);
        c.listen_watch = c.loop.watch(c.listen_fd, fd_interest::read);
        c.listen_conn = c.listen_watch.on_readable().connect([weak] {
            if (auto s = weak.lock())
                s->accept_ready();
        });

        c.sweeper = c.loop.every(c.cfg.sweep_interval);
        c.sweep_conn = c.sweeper.on_fire().connect([weak] {
            if (auto s = weak.lock())
                s->sweep();
        });
    }

    static std::uint16_t actual_port(int fd) noexcept
    {
        sockaddr_storage ss{};
        socklen_t sl = sizeof ss;
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &sl) != 0)
            return 0;
        if (ss.ss_family == AF_INET)
            return ntohs(reinterpret_cast<sockaddr_in*>(&ss)->sin_port);
        if (ss.ss_family == AF_INET6)
            return ntohs(reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port);
        return 0;
    }

    std::shared_ptr<detail::server_core> c_;
};

} // namespace http
} // namespace snicholls

#endif // SNICHOLLS_HAS_EVENT_LOOP
#endif /* http_server_hpp */
