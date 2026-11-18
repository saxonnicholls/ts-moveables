//
//  http2.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - HTTP/2 (RFC 9113) as a protocol delegate
//
//  The second protocol on the same two axes, and the one the interfaces were
//  shaped for. `deliver`, `respond`, `begin_stream` and `stream_write` have
//  carried a stream id since phase 1 - always 0 for HTTP/1.x - precisely so
//  that multiplexing needed no interface change. It did not: h2 streams map
//  straight onto the existing async `responder` and `response_stream`, which
//  already complete out of order because a moveable complete-once handle can
//  be answered from any thread at any time. That is the whole payoff of the
//  design, cashed in.
//
//      snicholls::http::server srv;
//      snicholls::http::enable_http2(srv);      // h2 by ALPN, h2c by preface
//      srv.get("/hello", [](const auto&, auto res) {
//          res.send(200, "text/plain", "hello");
//      });
//
//  Selection is per connection and at run time. `enable_http2` installs a
//  protocol factory that hands each new connection an `alpn_protocol`: a
//  three-line delegate that looks at what ALPN settled on, sniffs the client
//  preface when nothing did, and swaps itself out for `http2_protocol` or
//  `http1_protocol` through the same `switch_protocol` path WebSocket uses.
//  One binary serves both, chosen per connection, with no rebuild - which is
//  the thing a compile-time architecture structurally cannot do.
//
//  Two things carry most of the risk, so both are stated plainly:
//
//  HPACK (RFC 7541) is the bulk of the work and the least forgiving part of
//  it. The static table, the integer and string primitives, the dynamic table
//  with its eviction rules and the canonical Huffman code are all here, and
//  the Huffman tables are built by a `constexpr` function into `.rodata`
//  exactly as the HTTP/1.1 parser's character tables are - a decode step is
//  one indexed load, not a walk. The published RFC 7541 Appendix C vectors are
//  in the test suite because a decoder that only ever agrees with its own
//  encoder is a decoder that has not been checked.
//
//  Abuse limits are here from the first commit rather than after the first
//  incident. A decoded-header-list cap (the HPACK bomb), a bound on concurrent
//  streams, a cap on compressed CONTINUATION bytes (CVE-2024-27316) and token
//  buckets on RST_STREAM and on SETTINGS/PING (the 2023 Rapid Reset class) are
//  not extras: a multiplexed server without them is a DoS amplifier, because
//  the client can make it do unbounded work for a bounded number of bytes.
//
//  HPACK, the frame layout and the field rules are in h2_wire.hpp, shared
//  with the client (h2_client.hpp); this header is the server's state
//  machine over them.
//
//  POSIX only, like everything downstream of the reactor; on Windows this
//  header compiles to nothing and SNICHOLLS_HAS_HTTP2 is 0.
//

#ifndef http2_hpp
#define http2_hpp

#include "server.hpp"
#include "h2_wire.hpp"

#if !SNICHOLLS_HAS_HTTP_SERVER
#define SNICHOLLS_HAS_HTTP2 0
#else
#define SNICHOLLS_HAS_HTTP2 1

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace snicholls {
namespace http {

// Everything a connection is allowed to cost us. The defaults are deliberately
// conservative: an HTTP/2 server's whole risk profile is that one connection
// can ask for unbounded work, and every field here is a place someone has
// already found a way to do that.
struct h2_config {
    std::uint32_t header_table_size = 4096;         // SETTINGS_HEADER_TABLE_SIZE
    std::uint32_t max_concurrent_streams = 100;     // SETTINGS_MAX_CONCURRENT_STREAMS
    std::uint32_t initial_window_size = 65535;      // SETTINGS_INITIAL_WINDOW_SIZE
    std::uint32_t max_frame_size = 16384;           // SETTINGS_MAX_FRAME_SIZE
    std::uint32_t max_header_list_size = 32u * 1024;// SETTINGS_MAX_HEADER_LIST_SIZE
    std::uint32_t connection_window = 1024u * 1024; // our receive window, raised by WINDOW_UPDATE

    std::size_t max_header_fields = 128;            // decoded fields per request
    std::size_t max_header_block = 64u * 1024;      // *compressed* bytes across CONTINUATIONs

    // The outbound mirror of the flow-control problem, and the one that is
    // ours rather than the peer's. A client may legally open a stream, ask for
    // a large response, and then simply never send a WINDOW_UPDATE. Nothing in
    // the protocol obliges it to. Everything we are told to send but cannot
    // then sits in the stream's buffer.
    //
    // For a whole-body respond() that backlog is bounded by what the
    // application chose to allocate, the same argument the inbound side makes
    // for max_body: the ceiling is a configuration decision the application
    // already made. A *streaming* producer is different - it generates as it
    // goes, so without a limit the buffer grows for as long as the client is
    // willing to stay silent, which is forever and costs the client nothing.
    //
    // This is a budget for the whole connection, not per stream: a per-stream
    // limit multiplies by max_concurrent_streams, and 100 x 8 MiB is not a
    // bound anyone wanted. The stream whose write crosses the line is the one
    // reset; streams already inside the budget are left alone, because one
    // greedy exchange is not grounds to punish the rest of the connection.
    std::size_t max_outbound_buffer = 8u * 1024 * 1024;

    // Rapid Reset (2023) and its relatives: a client that can make the server
    // open and tear down streams, or answer control frames, faster than it can
    // send bytes has an amplifier. Both are token buckets - a burst is normal
    // traffic, a sustained rate is not.
    double rst_burst = 200.0;
    double rst_per_second = 20.0;
    double control_burst = 200.0;                   // SETTINGS and PING together
    double control_per_second = 50.0;
};

namespace detail {


// RFC 9113 §5.1. `idle` never appears in the map - a stream that has not been
// opened has no state to store - and `reserved` never appears at all, because
// we do not push.
enum class h2_stream_state { open, half_closed_remote, half_closed_local, closed };

struct h2_stream {
    std::uint32_t id = 0;
    h2_stream_state state = h2_stream_state::open;

    std::int64_t send_window = 65535;       // what the peer will let us send
    std::int64_t recv_window = 65535;       // what we will let the peer send

    request req;
    bool have_headers = false;              // the request head has been decoded
    bool head_request = false;
    bool delivered = false;
    bool awaiting = false;                  // handed to a handler, no answer yet
    bool streaming = false;                 // an open response_stream

    bool has_content_length = false;
    std::uint64_t content_length = 0;
    std::uint64_t body_bytes = 0;

    std::string out_buf;                    // response body the send window will not take yet
    std::size_t out_off = 0;
    bool out_end = false;                   // END_STREAM once out_buf drains
    bool end_sent = false;
    bool headers_sent = false;

    std::chrono::steady_clock::time_point opened{};
};

} // namespace detail

// ---------------------------------------------------------------- the protocol

class http2_protocol final : public protocol_delegate {
public:
    explicit http2_protocol(const parse_limits& lim, h2_config cfg = h2_config{},
                            bool preface_already_seen = false)
        : lim_(lim), cfg_(cfg), preface_seen_(preface_already_seen)
    {
        dec_.initial_table_size(cfg_.header_table_size);
        conn_recv_window_ = std::int64_t(std::max<std::uint32_t>(cfg_.connection_window, 65535));
        rst_bucket_.configure(cfg_.rst_burst, cfg_.rst_per_second);
        ctl_bucket_.configure(cfg_.control_burst, cfg_.control_per_second);
    }

    const char* name() const noexcept override { return "h2"; }
    bool close_after_flush() const noexcept override { return close_; }
    std::size_t last_response_bytes() const noexcept override { return last_bytes_; }

    // Any stream still owed an answer. The sweeper uses this to keep a
    // connection whose handler went off to a thread pool from being reaped.
    bool busy() const noexcept override
    {
        for (const auto& kv : streams_)
            if (kv.second->awaiting)
                return true;
        return false;
    }

    // Deliberately false, always. `receiving()` drives the request-timeout
    // sweep, and that sweep answers with a literal HTTP/1.1 408 - which on an
    // h2 connection is not a timeout, it is a framing error the client cannot
    // parse. Half-finished requests are timed out from inside consume()
    // instead, per stream and with RST_STREAM, which is both correct for the
    // protocol and better behaviour: one slow stream does not kill the other
    // ninety-nine multiplexed onto the same socket.
    bool receiving() const noexcept override { return false; }

    bool consume(std::string& in, connection_host& host) override
    {
        if (close_) {
            in.clear();
            return false;
        }
        if (!preface_seen_) {
            const std::size_t have = std::min(in.size(), kH2PrefaceLen);
            if (std::memcmp(in.data(), kH2Preface, have) != 0) {
                connection_error(host, h2_error::protocol_error, "bad connection preface");
                in.clear();
                return false;
            }
            if (in.size() < kH2PrefaceLen)
                return true;
            in.erase(0, kH2PrefaceLen);
            preface_seen_ = true;
        }
        if (!settings_sent_) {
            send_settings(host);
            settings_sent_ = true;
        }

        std::size_t off = 0;
        while (!close_) {
            if (in.size() - off < 9)
                break;
            const unsigned char* p = reinterpret_cast<const unsigned char*>(in.data()) + off;
            const detail::h2_frame_header fh = detail::h2_read_header(p);
            const std::uint32_t len = fh.length;
            const std::uint8_t type = fh.type;
            const std::uint8_t flags = fh.flags;
            const std::uint32_t sid = fh.stream;

            if (len > cfg_.max_frame_size) {
                connection_error(host, h2_error::frame_size_error, "frame exceeds SETTINGS_MAX_FRAME_SIZE");
                break;
            }
            if (in.size() - off < 9u + len)
                break;                          // the whole frame, or nothing

            if (!first_frame_seen_) {
                first_frame_seen_ = true;
                if (type != std::uint8_t(h2_frame::settings) || (flags & h2_flag_ack)) {
                    connection_error(host, h2_error::protocol_error,
                                     "the preface must be followed by SETTINGS");
                    break;
                }
            }
            // §6.10: a header block is atomic on the wire. Nothing at all may
            // come between HEADERS and its CONTINUATIONs - not another
            // stream's frames, not even PING - because the HPACK state would
            // be applied out of order.
            if (cont_stream_ &&
                (type != std::uint8_t(h2_frame::continuation) || sid != cont_stream_)) {
                connection_error(host, h2_error::protocol_error,
                                 "frame interleaved with a header block");
                break;
            }

            handle_frame(type, flags, sid, p + 9, len, host);
            off += 9u + len;
        }
        if (off)
            in.erase(0, off);

        expire_streams(host);
        flush_window_updates(host);
        pump(host);
        return !close_;
    }

    // ---- answering
    //
    // Every one of these is keyed by the stream id the interface has carried
    // since phase 1. Nothing here needed a new entry point, and nothing here
    // assumes the answers arrive in the order the requests did.

    void respond(std::uint64_t sid, response&& res, connection_host& host) override
    {
        last_bytes_ = 0;
        detail::h2_stream* s = find(std::uint32_t(sid));
        if (!s || s->end_sent)
            return;
        s->awaiting = false;
        const bool no_body = s->head_request || res.body.empty();
        // h2 frames carry the length, so Content-Length is not required here
        // the way it is on HTTP/1.1 - but clients and caches still read it,
        // and a HEAD answer has no other way to report the size it elided
        if (!res.headers.has("content-length") && res.status != 204 && res.status != 304 &&
            !(res.status >= 100 && res.status < 200)) {
            std::string n;
            detail::append_uint(n, res.body.size());
            res.headers.set("content-length", n);
        }
        send_response_headers(*s, res, no_body, host);
        if (!no_body) {
            s->out_buf = std::move(res.body);
            s->out_off = 0;
            s->out_end = true;
        } else {
            s->end_sent = true;
            close_local(*s);
        }
        last_bytes_ += pump(host);
    }

    bool begin_stream(std::uint64_t sid, response&& head, connection_host& host) override
    {
        last_bytes_ = 0;
        detail::h2_stream* s = find(std::uint32_t(sid));
        if (!s || s->end_sent)
            return false;
        s->awaiting = false;
        s->streaming = true;
        // No Content-Length and no chunked framing: h2 lengths are the frames
        send_response_headers(*s, head, /*end_stream*/ false, host);
        return true;
    }

    void stream_write(std::uint64_t sid, const char* data, std::size_t n,
                      connection_host& host) override
    {
        detail::h2_stream* s = find(std::uint32_t(sid));
        if (!s || !s->streaming || s->end_sent || n == 0 || s->head_request)
            return;
        compact(*s);
        s->out_buf.append(data, n);
        last_bytes_ += pump(host);
        // pump() has already sent everything the windows allowed, so what is
        // left is precisely what the peer will not take. Checking after rather
        // than before means a large write that goes straight out is never
        // refused for its size - only a genuine backlog is.
        //
        // `s` may not survive this call: stream_error forgets the stream.
        if (buffered_bytes() > cfg_.max_outbound_buffer)
            stream_error(host, std::uint32_t(sid), h2_error::enhance_your_calm);
    }

    void end_stream(std::uint64_t sid, connection_host& host) override
    {
        detail::h2_stream* s = find(std::uint32_t(sid));
        if (!s || !s->streaming || s->end_sent)
            return;
        s->streaming = false;
        s->out_end = true;
        last_bytes_ += pump(host);
    }

    // One stream fails; the connection and its other streams carry on
    bool abort_stream(std::uint64_t sid, connection_host& host) override
    {
        detail::h2_stream* s = find(std::uint32_t(sid));
        if (s && !s->end_sent)
            stream_error(host, std::uint32_t(sid), h2_error::internal_error);
        return true;
    }

    // Everything the send windows have refused so far, across every stream.
    //
    // Summed rather than carried as a running counter: the value is wanted
    // once per write, the stream count is capped by max_concurrent_streams,
    // and a counter would have to be kept in step with append, pump, compact
    // and erase - four places to drift out of agreement about the same number.
    std::size_t buffered_bytes() const noexcept override
    {
        std::size_t n = 0;
        for (const auto& kv : streams_)
            n += kv.second->out_buf.size() - kv.second->out_off;
        return n;
    }

    // ---- observation, for tests and metrics
    std::size_t open_streams() const noexcept { return streams_.size(); }
    std::int64_t connection_send_window() const noexcept { return conn_send_window_; }
    bool goaway_sent() const noexcept { return goaway_sent_; }
    h2_error last_error() const noexcept { return last_error_; }

private:
    // ------------------------------------------------------------- emitting

    void emit(connection_host& host, const std::string& frame)
    {
        host.write_app(frame.data(), frame.size());
        last_bytes_ += frame.size();
    }

    void send_settings(connection_host& host)
    {
        std::string o;
        const std::uint16_t ids[6] = {
            h2_settings_header_table_size, h2_settings_enable_push,
            h2_settings_max_concurrent_streams, h2_settings_initial_window_size,
            h2_settings_max_frame_size, h2_settings_max_header_list_size};
        const std::uint32_t vals[6] = {
            cfg_.header_table_size, 0u, cfg_.max_concurrent_streams,
            cfg_.initial_window_size, cfg_.max_frame_size, cfg_.max_header_list_size};
        detail::h2_put_header(o, 36, h2_frame::settings, 0, 0);
        for (int i = 0; i < 6; ++i) {
            o.push_back(char((ids[i] >> 8) & 0xff));
            o.push_back(char(ids[i] & 0xff));
            detail::h2_put32(o, vals[i]);
        }
        // The connection receive window starts at 65535 whatever SETTINGS
        // says; raising it takes a WINDOW_UPDATE on stream 0
        const std::int64_t want = std::int64_t(std::max<std::uint32_t>(cfg_.connection_window, 65535));
        if (want > 65535)
            detail::h2_put_window_update(o, 0, std::uint32_t(want - 65535));
        emit(host, o);
    }

    void send_rst(connection_host& host, std::uint32_t sid, h2_error code)
    {
        std::string o;
        detail::h2_put_rst(o, sid, code);
        emit(host, o);
    }

    void connection_error(connection_host& host, h2_error code, const char* why)
    {
        if (goaway_sent_)
            return;
        goaway_sent_ = true;
        close_ = true;
        last_error_ = code;
        std::string o;
        detail::h2_put_goaway(o, last_peer_stream_, code, why);
        emit(host, o);
    }

    // A stream error: the connection survives, this exchange does not
    void stream_error(connection_host& host, std::uint32_t sid, h2_error code)
    {
        send_rst(host, sid, code);
        forget(sid);
    }

    // ------------------------------------------------------------ streams

    detail::h2_stream* find(std::uint32_t sid) noexcept
    {
        auto it = streams_.find(sid);
        return it == streams_.end() ? nullptr : it->second.get();
    }

    // Streams are held by shared_ptr for one specific reason: `deliver` hands
    // the handler a `const request&` that lives inside the stream, and a
    // handler that answers inline gets all the way back round to closing that
    // stream while it is still holding the reference. A strong local reference
    // across the call means the object outlives the erase, and the handler
    // reading the request after answering is merely odd rather than undefined.
    std::shared_ptr<detail::h2_stream> hold(std::uint32_t sid) const
    {
        auto it = streams_.find(sid);
        return it == streams_.end() ? std::shared_ptr<detail::h2_stream>() : it->second;
    }

    void forget(std::uint32_t sid)
    {
        streams_.erase(sid);
        remember_closed(sid);
    }

    // A bounded memory of streams we have finished with, so that a frame
    // arriving after the close is answered STREAM_CLOSED rather than being
    // mistaken for a brand-new stream (§5.1) or for one that never existed
    void remember_closed(std::uint32_t sid)
    {
        if (std::find(closed_.begin(), closed_.end(), sid) != closed_.end())
            return;
        closed_.push_back(sid);
        if (closed_.size() > 256)
            closed_.pop_front();
    }

    bool was_closed(std::uint32_t sid) const noexcept
    {
        return std::find(closed_.begin(), closed_.end(), sid) != closed_.end();
    }

    void close_local(detail::h2_stream& s)
    {
        if (s.state == detail::h2_stream_state::half_closed_remote) {
            forget(s.id);
            return;
        }
        s.state = detail::h2_stream_state::half_closed_local;
    }

    static void compact(detail::h2_stream& s)
    {
        if (s.out_off && s.out_off * 2 >= s.out_buf.size()) {
            s.out_buf.erase(0, s.out_off);
            s.out_off = 0;
        }
    }

    // Half-finished requests are a per-stream problem, so they get a
    // per-stream answer. Checked here rather than from the connection sweeper
    // because the sweeper's remedy is an HTTP/1.1 error line.
    void expire_streams(connection_host& host)
    {
        const auto limit = host.config().request_timeout;
        if (close_ || limit.count() <= 0 || streams_.empty())
            return;
        const auto now = std::chrono::steady_clock::now();
        std::vector<std::uint32_t> doomed;
        for (auto& kv : streams_) {
            detail::h2_stream& s = *kv.second;
            if (s.delivered || s.awaiting)
                continue;                       // a handler owns it now
            if (now - s.opened > limit)
                doomed.push_back(s.id);
        }
        for (std::uint32_t sid : doomed)
            stream_error(host, sid, h2_error::enhance_your_calm);
    }

    // ------------------------------------------------------ inbound windows

    // Replenish eagerly. We buffer whole request bodies up to the parser's
    // max_body, so holding the window closed would stall the peer for no
    // benefit; the real bound on memory is max_body times max_concurrent
    // streams, which is a configuration decision rather than a window one.
    void consumed(std::uint32_t sid, std::uint32_t n)
    {
        conn_recv_window_ -= std::int64_t(n);
        conn_pending_update_ += n;
        auto it = streams_.find(sid);
        if (it != streams_.end()) {
            it->second->recv_window -= std::int64_t(n);
            stream_updates_[sid] += n;
        }
    }

    void flush_window_updates(connection_host& host)
    {
        if (close_)
            return;
        std::string o;
        if (conn_pending_update_) {
            detail::h2_put_window_update(o, 0, conn_pending_update_);
            conn_recv_window_ += std::int64_t(conn_pending_update_);
            conn_pending_update_ = 0;
        }
        for (auto& kv : stream_updates_) {
            auto it = streams_.find(kv.first);
            if (it == streams_.end() || kv.second == 0)
                continue;
            detail::h2_put_window_update(o, kv.first, kv.second);
            it->second->recv_window += std::int64_t(kv.second);
        }
        stream_updates_.clear();
        if (!o.empty())
            host.write_app(o.data(), o.size());
    }

    // ----------------------------------------------------- outbound windows

    // Sends whatever the windows currently allow, across every stream that has
    // something queued, and returns how many bytes went out. The count matters
    // only to the response path: the access log attributes bytes to the
    // exchange that produced them, and a pump driven by an incoming
    // WINDOW_UPDATE belongs to no exchange in particular.
    std::size_t pump(connection_host& host)
    {
        if (close_)
            return 0;
        std::string o;
        for (auto it = streams_.begin(); it != streams_.end();) {
            detail::h2_stream& s = *it->second;
            bool finished = false;
            while (!s.end_sent) {
                const std::size_t left = s.out_buf.size() - s.out_off;
                if (left == 0) {
                    if (!s.out_end)
                        break;                  // a streaming producer has more to come
                    detail::h2_put_header(o, 0, h2_frame::data, h2_flag_end_stream, s.id);
                    s.end_sent = true;
                    break;
                }
                const std::int64_t window = std::min(conn_send_window_, s.send_window);
                if (window <= 0)
                    break;                      // blocked; a WINDOW_UPDATE will restart us
                std::size_t n = std::min<std::size_t>(left, std::size_t(window));
                n = std::min<std::size_t>(n, peer_max_frame_);
                const bool last = (n == left) && s.out_end;
                detail::h2_put_header(o, std::uint32_t(n), h2_frame::data, last ? h2_flag_end_stream : 0, s.id);
                o.append(s.out_buf, s.out_off, n);
                s.out_off += n;
                conn_send_window_ -= std::int64_t(n);
                s.send_window -= std::int64_t(n);
                if (last)
                    s.end_sent = true;
            }
            compact(s);                         // before any erase: `s` may not survive it
            if (s.end_sent && !s.awaiting && !s.streaming) {
                if (s.state == detail::h2_stream_state::half_closed_remote) {
                    remember_closed(s.id);
                    it = streams_.erase(it);
                    finished = true;
                } else {
                    s.state = detail::h2_stream_state::half_closed_local;
                }
            }
            if (!finished)
                ++it;
        }
        if (!o.empty())
            host.write_app(o.data(), o.size());
        return o.size();
    }

    // -------------------------------------------------------- frame handling

    void handle_frame(std::uint8_t type, std::uint8_t flags, std::uint32_t sid,
                      const unsigned char* p, std::uint32_t len, connection_host& host)
    {
        switch (type) {
        case std::uint8_t(h2_frame::data):          on_data(flags, sid, p, len, host); break;
        case std::uint8_t(h2_frame::headers):       on_headers(flags, sid, p, len, host); break;
        case std::uint8_t(h2_frame::priority):      on_priority(sid, p, len, host); break;
        case std::uint8_t(h2_frame::rst_stream):    on_rst(sid, p, len, host); break;
        case std::uint8_t(h2_frame::settings):      on_settings(flags, sid, p, len, host); break;
        case std::uint8_t(h2_frame::push_promise):
            // §8.4: a client must never send one, and a server that tolerates
            // it has handed the client a way to create server-side state
            connection_error(host, h2_error::protocol_error, "PUSH_PROMISE from a client");
            break;
        case std::uint8_t(h2_frame::ping):          on_ping(flags, sid, p, len, host); break;
        case std::uint8_t(h2_frame::goaway):        on_goaway(sid, len, host); break;
        case std::uint8_t(h2_frame::window_update): on_window_update(sid, p, len, host); break;
        case std::uint8_t(h2_frame::continuation):  on_continuation(flags, sid, p, len, host); break;
        default:
            // §5.5: an unknown frame type is discarded, which is what lets the
            // protocol be extended without breaking us
            break;
        }
    }

    void on_data(std::uint8_t flags, std::uint32_t sid, const unsigned char* p,
                 std::uint32_t len, connection_host& host)
    {
        if (sid == 0) {
            connection_error(host, h2_error::protocol_error, "DATA on stream 0");
            return;
        }
        // Flow control is accounted before anything else can reject the frame:
        // the peer has spent the window whether or not we like the stream
        consumed(sid, len);
        if (conn_recv_window_ < 0) {
            connection_error(host, h2_error::flow_control_error, "connection receive window exceeded");
            return;
        }

        std::uint32_t off = 0, pad = 0;
        if (flags & h2_flag_padded) {
            if (len < 1) {
                connection_error(host, h2_error::protocol_error, "padded DATA with no pad length");
                return;
            }
            pad = p[0];
            off = 1;
            if (std::uint32_t(pad) + 1u > len) {
                connection_error(host, h2_error::protocol_error, "DATA padding exceeds the frame");
                return;
            }
        }

        detail::h2_stream* s = find(sid);
        if (!s) {
            if (was_closed(sid) || sid <= last_peer_stream_)
                stream_error(host, sid, h2_error::stream_closed);
            else
                connection_error(host, h2_error::protocol_error, "DATA on an idle stream");
            return;
        }
        if (s->state != detail::h2_stream_state::open &&
            s->state != detail::h2_stream_state::half_closed_local) {
            stream_error(host, sid, h2_error::stream_closed);
            return;
        }
        if (s->recv_window < 0) {
            stream_error(host, sid, h2_error::flow_control_error);
            return;
        }

        const std::uint32_t n = len - off - pad;
        if (s->req.body.size() + n > lim_.max_body) {
            stream_error(host, sid, h2_error::enhance_your_calm);
            return;
        }
        s->req.body.append(reinterpret_cast<const char*>(p + off), n);
        s->body_bytes += n;

        if (flags & h2_flag_end_stream)
            end_of_request(sid, host);
    }

    void on_headers(std::uint8_t flags, std::uint32_t sid, const unsigned char* p,
                    std::uint32_t len, connection_host& host)
    {
        if (sid == 0) {
            connection_error(host, h2_error::protocol_error, "HEADERS on stream 0");
            return;
        }
        if ((sid & 1u) == 0) {
            connection_error(host, h2_error::protocol_error, "even stream identifier from a client");
            return;
        }

        std::uint32_t off = 0, pad = 0;
        if (flags & h2_flag_padded) {
            if (len < 1) {
                connection_error(host, h2_error::protocol_error, "padded HEADERS with no pad length");
                return;
            }
            pad = p[0];
            off = 1;
        }
        if (flags & h2_flag_priority) {
            if (len < off + 5u) {
                connection_error(host, h2_error::frame_size_error, "HEADERS priority block truncated");
                return;
            }
            const std::uint32_t dep = detail::h2_get32(p + off) & 0x7fffffffu;
            off += 5;
            if (dep == sid) {
                // §5.3.1 in RFC 7540; priority is deprecated by RFC 9113 but a
                // self-dependency is still nonsense and still worth refusing
                stream_error(host, sid, h2_error::protocol_error);
                return;
            }
        }
        if (off + pad > len) {
            connection_error(host, h2_error::protocol_error, "HEADERS padding exceeds the frame");
            return;
        }

        detail::h2_stream* s = find(sid);
        if (s) {
            if (s->state != detail::h2_stream_state::open) {
                stream_error(host, sid, h2_error::stream_closed);
                return;
            }
            if (!(flags & h2_flag_end_stream)) {
                // Trailers arrive as HEADERS with END_STREAM; anything else on
                // an open stream is a second request head, which is malformed
                stream_error(host, sid, h2_error::protocol_error);
                return;
            }
            trailers_ = true;                   // decoded for HPACK state, then discarded
        } else {
            if (was_closed(sid) || sid <= last_peer_stream_) {
                // §5.1.1: identifiers only ever go up, and a reused one is a
                // desynchronised peer rather than a new request
                connection_error(host, h2_error::protocol_error,
                                 "stream identifier reused or out of order");
                return;
            }
            if (goaway_received_) {
                stream_error(host, sid, h2_error::refused_stream);
                return;
            }
            if (streams_.size() >= cfg_.max_concurrent_streams) {
                send_rst(host, sid, h2_error::refused_stream);
                last_peer_stream_ = sid;
                remember_closed(sid);
                return;
            }
            trailers_ = false;
            auto ns = std::make_shared<detail::h2_stream>();
            ns->id = sid;
            ns->send_window = std::int64_t(peer_initial_window_);
            ns->recv_window = std::int64_t(cfg_.initial_window_size);
            ns->opened = std::chrono::steady_clock::now();
            s = ns.get();
            streams_.emplace(sid, std::move(ns));
        }
        last_peer_stream_ = std::max(last_peer_stream_, sid);

        if (len - off - pad > cfg_.max_header_block) {
            // Only reachable when the compressed cap is configured below
            // SETTINGS_MAX_FRAME_SIZE, but the two are independent knobs and
            // the check belongs wherever the block grows
            connection_error(host, h2_error::enhance_your_calm, "header block too large");
            return;
        }
        block_.assign(reinterpret_cast<const char*>(p + off), len - off - pad);
        block_stream_ = sid;
        block_end_stream_ = (flags & h2_flag_end_stream) != 0;
        if (flags & h2_flag_end_headers) {
            cont_stream_ = 0;
            finish_header_block(host);
        } else {
            cont_stream_ = sid;
        }
    }

    void on_continuation(std::uint8_t flags, std::uint32_t sid, const unsigned char* p,
                         std::uint32_t len, connection_host& host)
    {
        if (cont_stream_ == 0 || sid != cont_stream_) {
            connection_error(host, h2_error::protocol_error, "CONTINUATION without HEADERS");
            return;
        }
        // The CONTINUATION flood (CVE-2024-27316): an unbounded run of frames
        // that never sets END_HEADERS costs the attacker almost nothing and
        // costs us memory and HPACK work without ever creating a request the
        // rest of the server could account for.
        if (block_.size() + len > cfg_.max_header_block) {
            connection_error(host, h2_error::enhance_your_calm, "header block too large");
            return;
        }
        block_.append(reinterpret_cast<const char*>(p), len);
        if (flags & h2_flag_end_headers) {
            cont_stream_ = 0;
            finish_header_block(host);
        }
    }

    void on_priority(std::uint32_t sid, const unsigned char* p, std::uint32_t len,
                     connection_host& host)
    {
        if (sid == 0) {
            connection_error(host, h2_error::protocol_error, "PRIORITY on stream 0");
            return;
        }
        if (len != 5) {
            stream_error(host, sid, h2_error::frame_size_error);
            return;
        }
        if ((detail::h2_get32(p) & 0x7fffffffu) == sid) {
            stream_error(host, sid, h2_error::protocol_error);
            return;
        }
        // Parsed, validated and ignored. RFC 9113 deprecates the priority
        // scheme outright; honouring it would be implementing a mechanism the
        // standard has withdrawn.
    }

    void on_rst(std::uint32_t sid, const unsigned char* p, std::uint32_t len,
                connection_host& host)
    {
        (void)p;
        if (sid == 0) {
            connection_error(host, h2_error::protocol_error, "RST_STREAM on stream 0");
            return;
        }
        if (len != 4) {
            connection_error(host, h2_error::frame_size_error, "RST_STREAM length is not 4");
            return;
        }
        detail::h2_stream* s = find(sid);
        if (!s && !was_closed(sid) && sid > last_peer_stream_) {
            connection_error(host, h2_error::protocol_error, "RST_STREAM on an idle stream");
            return;
        }
        // Rapid Reset: open a stream, cancel it, repeat. Each cycle is nine
        // bytes to the client and a whole request to us, so the only defence
        // that works is a rate limit on the cancels themselves.
        if (!rst_bucket_.take(std::chrono::steady_clock::now())) {
            connection_error(host, h2_error::enhance_your_calm, "RST_STREAM flood");
            return;
        }
        if (s)
            forget(sid);
    }

    void on_settings(std::uint8_t flags, std::uint32_t sid, const unsigned char* p,
                     std::uint32_t len, connection_host& host)
    {
        if (sid != 0) {
            connection_error(host, h2_error::protocol_error, "SETTINGS on a non-zero stream");
            return;
        }
        if (flags & h2_flag_ack) {
            if (len != 0)
                connection_error(host, h2_error::frame_size_error, "SETTINGS ACK with a payload");
            return;
        }
        if (len % 6 != 0) {
            connection_error(host, h2_error::frame_size_error, "SETTINGS length is not a multiple of 6");
            return;
        }
        // Every SETTINGS obliges us to send an ACK, which is the amplification
        if (!ctl_bucket_.take(std::chrono::steady_clock::now())) {
            connection_error(host, h2_error::enhance_your_calm, "SETTINGS flood");
            return;
        }

        for (std::uint32_t i = 0; i + 6 <= len; i += 6) {
            const std::uint16_t id = std::uint16_t((std::uint32_t(p[i]) << 8) | p[i + 1]);
            const std::uint32_t v = detail::h2_get32(p + i + 2);
            switch (id) {
            case h2_settings_header_table_size:
                enc_.peer_table_size(v);
                break;
            case h2_settings_enable_push:
                if (v > 1) {
                    connection_error(host, h2_error::protocol_error, "SETTINGS_ENABLE_PUSH is not 0 or 1");
                    return;
                }
                break;
            case h2_settings_max_concurrent_streams:
                break;                          // a limit on what we may push; we never do
            case h2_settings_initial_window_size: {
                if (v > 0x7fffffffu) {
                    connection_error(host, h2_error::flow_control_error,
                                     "SETTINGS_INITIAL_WINDOW_SIZE above 2^31-1");
                    return;
                }
                // §6.9.2: the change is a *delta* applied to every open
                // stream, not a reset - a stream that has already sent data
                // keeps that spend
                const std::int64_t delta = std::int64_t(v) - std::int64_t(peer_initial_window_);
                peer_initial_window_ = v;
                for (auto& kv : streams_) {
                    kv.second->send_window += delta;
                    if (kv.second->send_window > 0x7fffffffLL) {
                        connection_error(host, h2_error::flow_control_error,
                                         "stream send window overflowed");
                        return;
                    }
                }
                break;
            }
            case h2_settings_max_frame_size:
                if (v < 16384u || v > 16777215u) {
                    connection_error(host, h2_error::protocol_error, "SETTINGS_MAX_FRAME_SIZE out of range");
                    return;
                }
                peer_max_frame_ = v;
                break;
            case h2_settings_max_header_list_size:
                peer_max_header_list_ = v;
                break;
            default:
                break;                          // §6.5.2: unknown settings are ignored
            }
        }

        std::string o;
        detail::h2_put_header(o, 0, h2_frame::settings, h2_flag_ack, 0);
        emit(host, o);
        pump(host);
    }

    void on_ping(std::uint8_t flags, std::uint32_t sid, const unsigned char* p,
                 std::uint32_t len, connection_host& host)
    {
        if (sid != 0) {
            connection_error(host, h2_error::protocol_error, "PING on a non-zero stream");
            return;
        }
        if (len != 8) {
            connection_error(host, h2_error::frame_size_error, "PING length is not 8");
            return;
        }
        if (flags & h2_flag_ack)
            return;
        if (!ctl_bucket_.take(std::chrono::steady_clock::now())) {
            connection_error(host, h2_error::enhance_your_calm, "PING flood");
            return;
        }
        std::string o;
        detail::h2_put_header(o, 8, h2_frame::ping, h2_flag_ack, 0);
        o.append(reinterpret_cast<const char*>(p), 8);
        emit(host, o);
    }

    void on_goaway(std::uint32_t sid, std::uint32_t len, connection_host& host)
    {
        if (sid != 0) {
            connection_error(host, h2_error::protocol_error, "GOAWAY on a non-zero stream");
            return;
        }
        if (len < 8) {
            connection_error(host, h2_error::frame_size_error, "GOAWAY shorter than 8 octets");
            return;
        }
        // Finish what is in flight, refuse anything new, then close
        goaway_received_ = true;
        if (streams_.empty())
            close_ = true;
    }

    void on_window_update(std::uint32_t sid, const unsigned char* p, std::uint32_t len,
                          connection_host& host)
    {
        if (len != 4) {
            connection_error(host, h2_error::frame_size_error, "WINDOW_UPDATE length is not 4");
            return;
        }
        const std::uint32_t inc = detail::h2_get32(p) & 0x7fffffffu;
        if (sid == 0) {
            if (inc == 0) {
                connection_error(host, h2_error::protocol_error, "WINDOW_UPDATE increment of 0");
                return;
            }
            conn_send_window_ += std::int64_t(inc);
            if (conn_send_window_ > 0x7fffffffLL) {
                connection_error(host, h2_error::flow_control_error, "connection send window overflowed");
                return;
            }
            pump(host);
            return;
        }
        detail::h2_stream* s = find(sid);
        if (!s) {
            if (was_closed(sid) || sid <= last_peer_stream_)
                return;                         // a window for a stream that is over: harmless
            connection_error(host, h2_error::protocol_error, "WINDOW_UPDATE on an idle stream");
            return;
        }
        if (inc == 0) {
            stream_error(host, sid, h2_error::protocol_error);
            return;
        }
        s->send_window += std::int64_t(inc);
        if (s->send_window > 0x7fffffffLL) {
            stream_error(host, sid, h2_error::flow_control_error);
            return;
        }
        pump(host);
    }

    // ------------------------------------------------------- header blocks

    void finish_header_block(connection_host& host)
    {
        const std::uint32_t sid = block_stream_;
        fields_.clear();
        const unsigned char* b = reinterpret_cast<const unsigned char*>(block_.data());
        const hpack::status st = dec_.decode(b, b + block_.size(), fields_,
                                             cfg_.max_header_list_size, cfg_.max_header_fields);
        block_.clear();
        block_stream_ = 0;

        if (st == hpack::status::compression_error) {
            // §4.3: HPACK state is shared with the peer's encoder, so a bad
            // block is not a bad request - it is a connection that can never
            // be interpreted again
            connection_error(host, h2_error::compression_error, "HPACK decoding failed");
            return;
        }

        detail::h2_stream* s = find(sid);
        if (!s)
            return;                             // reset while its header block was in flight

        if (st == hpack::status::list_too_large) {
            stream_error(host, sid, h2_error::enhance_your_calm);
            return;
        }
        if (trailers_) {
            trailers_ = false;
            end_of_request(sid, host);           // trailers are validated by HPACK and discarded
            return;
        }
        if (!build_request(*s)) {
            stream_error(host, sid, h2_error::protocol_error);
            return;
        }
        s->have_headers = true;
        if (block_end_stream_)
            end_of_request(sid, host);
    }

    // RFC 9113 §8.3: what makes a request malformed. Every one of these is a
    // request-smuggling vector when an h2 front end talks HTTP/1.1 to a back
    // end, which is why they are refused here rather than normalised.
    bool build_request(detail::h2_stream& s)
    {
        std::string method, scheme, path, authority;
        bool have_method = false, have_scheme = false, have_path = false, have_authority = false;
        bool seen_regular = false;
        std::string cookie;
        bool have_cookie = false;

        request& req = s.req;
        req.clear();
        req.http_major = 2;
        req.http_minor = 0;
        req.keep_alive = true;

        for (const auto& f : fields_) {
            const std::string& n = f.first;
            const std::string& v = f.second;
            if (n.empty())
                return false;
            if (!detail::h2_valid_value(v))
                return false;

            if (n[0] == ':') {
                if (seen_regular)
                    return false;               // pseudo-headers come first, always
                if (n == ":method") {
                    if (have_method) return false;
                    have_method = true;
                    method = v;
                } else if (n == ":scheme") {
                    if (have_scheme) return false;
                    have_scheme = true;
                    scheme = v;
                } else if (n == ":path") {
                    if (have_path) return false;
                    have_path = true;
                    path = v;
                } else if (n == ":authority") {
                    if (have_authority) return false;
                    have_authority = true;
                    authority = v;
                } else {
                    return false;               // ":status" or anything unknown
                }
                continue;
            }

            seen_regular = true;
            if (!detail::h2_valid_name(n))
                return false;
            // §8.2.2: these have no meaning without HTTP/1.1's connection
            // model, and tolerating them is how a desync gets built
            if (detail::h2_connection_specific(n))
                return false;
            if (n == "te" && v != "trailers")
                return false;
            if (n == "cookie") {
                // §8.2.3: crumbs are rejoined so a handler sees one field
                if (have_cookie)
                    cookie += "; ";
                cookie += v;
                have_cookie = true;
                continue;
            }
            if (n == "content-length") {
                unsigned long long declared = 0;
                if (!detail::h2_parse_uint(v, declared))
                    return false;
                if (s.has_content_length && s.content_length != declared)
                    return false;
                s.has_content_length = true;
                s.content_length = declared;
            }
            req.headers.add(n, v);
        }

        if (have_cookie)
            req.headers.add("cookie", cookie);

        if (!have_method || method.empty())
            return false;
        if (method == "CONNECT") {
            // The CONNECT form has no :scheme and no :path, and we do not
            // proxy - refuse it rather than half-support it
            return false;
        }
        if (!have_scheme || scheme.empty())
            return false;
        if (!have_path || path.empty())
            return false;

        req.method_text = method;
        req.method = parse_method(method);
        s.head_request = (req.method == method::head);
        req.target = path;

        if (path == "*") {
            req.path = "*";
        } else {
            if (path[0] != '/')
                return false;
            const std::size_t q = path.find('?');
            const std::size_t plen = (q == std::string::npos) ? path.size() : q;
            if (q != std::string::npos)
                req.query.assign(path, q + 1, path.size() - q - 1);
            if (!detail::percent_decode(path.data(), plen, req.path, false))
                return false;
            if (req.path.find('\0') != std::string::npos)
                return false;
        }
        // Handlers written against HTTP/1.1 read Host; :authority is the same
        // information under a new name, so give them both
        if (have_authority && !authority.empty() && !req.headers.has("host"))
            req.headers.add("host", authority);
        return true;
    }

    void end_of_request(std::uint32_t sid, connection_host& host)
    {
        // A strong reference for the whole call: everything below can end up
        // erasing this stream from the map, including the handler answering
        // inline, and `req` is handed out by reference
        std::shared_ptr<detail::h2_stream> keep = hold(sid);
        if (!keep)
            return;
        detail::h2_stream& s = *keep;

        if (s.state == detail::h2_stream_state::open)
            s.state = detail::h2_stream_state::half_closed_remote;
        else if (s.state == detail::h2_stream_state::half_closed_local)
            s.state = detail::h2_stream_state::closed;

        if (s.delivered)
            return;
        if (!s.have_headers) {
            stream_error(host, sid, h2_error::protocol_error);
            return;
        }
        // §8.1.2.6: a body that does not match its declared length is
        // malformed, and it is malformed at exactly the point where a proxy
        // would otherwise re-frame it wrongly
        if (s.has_content_length && s.content_length != s.body_bytes) {
            stream_error(host, sid, h2_error::protocol_error);
            return;
        }

        s.delivered = true;
        s.awaiting = true;
        host.deliver(s.req, sid);
        // The handler may have answered inline, in which case the stream is
        // already finished and may already be out of the map
        if (s.end_sent && !s.streaming &&
            s.state == detail::h2_stream_state::half_closed_remote)
            forget(sid);
    }

    // ------------------------------------------------------------ responses

    void send_response_headers(detail::h2_stream& s, const response& res, bool end_stream,
                               connection_host& host)
    {
        if (s.headers_sent)
            return;
        s.headers_sent = true;

        fields_out_.clear();
        char code[8];
        const int status = (res.status >= 100 && res.status <= 599) ? res.status : 500;
        std::snprintf(code, sizeof code, "%d", status);
        fields_out_.emplace_back(":status", code);

        bool have_date = false, have_server = false;
        for (const auto& h : res.headers) {
            std::string n = h.first;
            detail::ascii_lower(n);
            // Connection-specific fields have no meaning here and RFC 9113
            // §8.2.2 forbids sending them
            if (detail::h2_connection_specific(n))
                continue;
            if (n == "date") have_date = true;
            if (n == "server") have_server = true;
            fields_out_.emplace_back(std::move(n), detail::h2_sanitise(h.second));
        }
        if (!have_date)
            fields_out_.emplace_back("date", host.http_date());
        if (!have_server)
            fields_out_.emplace_back("server", detail::h2_sanitise(host.config().server_name));

        std::string block;
        enc_.encode(fields_out_, block);

        // A header block larger than one frame becomes HEADERS plus
        // CONTINUATIONs, cut where the peer's own max_frame_size says
        std::string o;
        detail::h2_put_header_block(o, block, peer_max_frame_, end_stream, s.id);
        emit(host, o);
    }

    // ---------------------------------------------------------------- state

    parse_limits lim_;
    h2_config cfg_;

    hpack::decoder dec_;
    hpack::encoder enc_;
    std::vector<hpack::field> fields_;
    std::vector<hpack::field> fields_out_;

    std::map<std::uint32_t, std::shared_ptr<detail::h2_stream>> streams_;
    std::deque<std::uint32_t> closed_;
    std::map<std::uint32_t, std::uint32_t> stream_updates_;

    std::string block_;                     // the header block being assembled
    std::uint32_t block_stream_ = 0;
    std::uint32_t cont_stream_ = 0;         // non-zero while CONTINUATIONs are owed
    bool block_end_stream_ = false;
    bool trailers_ = false;

    std::uint32_t last_peer_stream_ = 0;
    std::uint32_t peer_max_frame_ = 16384;
    std::uint32_t peer_initial_window_ = 65535;
    std::uint32_t peer_max_header_list_ = 0xffffffffu;

    std::int64_t conn_send_window_ = 65535;
    std::int64_t conn_recv_window_ = 65535;
    std::uint32_t conn_pending_update_ = 0;

    detail::h2_bucket rst_bucket_;
    detail::h2_bucket ctl_bucket_;

    std::size_t last_bytes_ = 0;
    bool preface_seen_ = false;
    bool settings_sent_ = false;
    bool first_frame_seen_ = false;
    bool goaway_sent_ = false;
    bool goaway_received_ = false;
    bool close_ = false;
    h2_error last_error_ = h2_error::no_error;
};

// ---------------------------------------------------------- protocol selection
//
// The delegate that makes one binary serve both. It owns the connection for
// exactly as long as it takes to decide - ALPN if the transport negotiated
// one, the client preface if it did not - and then hands over through the same
// switch_protocol path the WebSocket upgrade uses.

class alpn_protocol final : public protocol_delegate {
public:
    explicit alpn_protocol(parse_limits lim, h2_config cfg = h2_config{},
                           bool allow_prior_knowledge = true)
        : lim_(lim), cfg_(cfg), prior_knowledge_(allow_prior_knowledge) {}

    const char* name() const noexcept override { return "alpn"; }
    void respond(std::uint64_t, response&&, connection_host&) override {}
    bool close_after_flush() const noexcept override { return false; }
    bool busy() const noexcept override { return false; }
    bool receiving() const noexcept override { return false; }

    bool consume(std::string& in, connection_host& host) override
    {
        const char* negotiated = host.alpn();
        if (negotiated && std::strcmp(negotiated, "h2") == 0) {
            // ALPN said h2, so the preface is mandatory and http2_protocol
            // will reject anything else - no sniffing required
            host.switch_protocol(std::unique_ptr<protocol_delegate>(
                new http2_protocol(lim_, cfg_)));
            return true;
        }
        if (negotiated && *negotiated) {        // http/1.1, or anything else we were offered
            host.switch_protocol(std::unique_ptr<protocol_delegate>(new http1_protocol(lim_)));
            return true;
        }
        if (in.empty())
            return true;                        // nothing negotiated and nothing to look at yet

        if (prior_knowledge_) {
            const std::size_t have = std::min(in.size(), kH2PrefaceCommit);
            if (std::memcmp(in.data(), kH2Preface, have) == 0) {
                if (have < kH2PrefaceCommit)
                    return true;                // still could be either; wait for the rest
                // Committed. The remaining octets are http2_protocol's to
                // check, so a preface that goes wrong late is a GOAWAY.
                host.switch_protocol(std::unique_ptr<protocol_delegate>(
                    new http2_protocol(lim_, cfg_)));
                return true;
            }
        }
        host.switch_protocol(std::unique_ptr<protocol_delegate>(new http1_protocol(lim_)));
        return true;
    }

private:
    parse_limits lim_;
    h2_config cfg_;
    bool prior_knowledge_;
};

// Serve HTTP/2 as well as HTTP/1.1 on this server, chosen per connection.
//
//   over TLS   the transport's ALPN decides; offer {"h2", "http/1.1"} there
//   plaintext  a client that opens with the h2 preface gets h2c, everyone
//              else gets HTTP/1.1
//
// Upgrade-based h2c (the `Upgrade: h2c` header on an HTTP/1.1 request) is
// deliberately not implemented: RFC 9113 removed it, no browser ever shipped
// it, and it is the one h2 entry point that has to reconstruct a request from
// a base64 SETTINGS payload - risk with no callers.
inline void enable_http2(server& srv, h2_config cfg = h2_config{},
                         bool allow_prior_knowledge = true)
{
    srv.protocol_factory([cfg, allow_prior_knowledge](const server_config& sc) {
        return std::unique_ptr<protocol_delegate>(
            new alpn_protocol(sc.limits, cfg, allow_prior_knowledge));
    });
}

// Speak HTTP/2 and nothing else: every connection is h2 from its first octet,
// and an HTTP/1.1 request gets a GOAWAY rather than a reply.
//
// This is the right mode behind a TLS-terminating proxy that has already done
// the ALPN, and it is also the mode h2spec's cleartext run assumes - which is
// worth stating plainly, because it is the one place the two modes cannot
// agree. Given "INVALID CONNECTION PREFACE" on a mixed listener, the honest
// answer is an HTTP/1.1 400: those bytes are a malformed HTTP/1.1 request, and
// the only reason to call them a malformed HTTP/2 preface instead is that we
// guessed. An h2-only listener has no guess to make, so it answers GOAWAY.
inline void enable_http2_only(server& srv, h2_config cfg = h2_config{})
{
    srv.protocol_factory([cfg](const server_config& sc) {
        return std::unique_ptr<protocol_delegate>(new http2_protocol(sc.limits, cfg));
    });
}

} // namespace http
} // namespace snicholls

#endif // SNICHOLLS_HAS_HTTP_SERVER
#endif /* http2_hpp */
