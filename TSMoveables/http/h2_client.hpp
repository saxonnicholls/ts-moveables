//
//  h2_client.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - the client side of an HTTP/2 connection (RFC 9113),
//  as a state machine with no I/O of its own:
//
//      snicholls::http::h2_client_session s;
//      s.start();                                        // preface and SETTINGS
//      const auto id = s.submit("GET", "https", "example.com", "/", {}, {});
//      write(s.take_output());                           // bytes for the socket
//      ...
//      s.consume(bytes_read);                            // frames from the server
//      write(s.take_output());                           // ACKs, WINDOW_UPDATEs, more body
//      for (auto& c : s.take_completions()) ...          // finished exchanges
//
//  http::client drives one of these per HTTP/2 connection; it is public so it
//  can be tested frame by frame, and so a protocol on top of h2 (gRPC) can
//  drive one directly. Bytes go in, bytes and completions come out, and
//  nothing calls back into the owner, so the owner can submit, cancel or
//  destroy from anywhere in its own handling without reentrancy.
//
//  It shares everything that is not a matter of side with the server
//  (h2_wire.hpp): HPACK, the frame layout, the field rules. What is the
//  client's alone:
//
//    - Streams are ours to open (odd, ascending) and the server's to answer.
//      A HEADERS frame on an even or never-opened stream, or a PUSH_PROMISE
//      (push is refused in our SETTINGS), is a connection error.
//    - A response head is :status and fields; 1xx heads are skipped; a second
//      HEADERS with END_STREAM carries the trailers - gRPC's status lives
//      there - kept in response::trailers.
//    - A stream the server did not process - REFUSED_STREAM, or above the last
//      stream a GOAWAY names - completes as `refused`, which RFC 9113 §8.7
//      says is safe to retry whatever the method.
//    - A header block for a stream we have already cancelled is still decoded
//      and then discarded: HPACK state is shared with the server's encoder,
//      and skipping one block corrupts every block after it.
//    - Bodies are buffered whole up to max_body, so receive windows are
//      replenished as data arrives; request bodies go out under the server's
//      windows.
//
//  C++17, standard library only. POSIX is not needed here; http::client,
//  which supplies the socket, is.
//

#ifndef ts_moveables_http_h2_client_hpp
#define ts_moveables_http_h2_client_hpp

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "h2_wire.hpp"
#include "message.hpp"

namespace snicholls {
namespace http {

struct h2_client_config {
    std::uint32_t header_table_size = 4096;           // SETTINGS_HEADER_TABLE_SIZE: the decoder's table
    std::uint32_t initial_window_size = 1u << 20;     // SETTINGS_INITIAL_WINDOW_SIZE: per stream, receiving
    std::uint32_t connection_window = 4u << 20;       // the connection's receive window
    std::uint32_t max_frame_size = 16384;             // SETTINGS_MAX_FRAME_SIZE: the largest frame we accept
    std::uint32_t max_header_list_size = 64u * 1024;  // SETTINGS_MAX_HEADER_LIST_SIZE: decoded, per response
    std::size_t max_header_fields = 256;
    std::size_t max_header_block = 256u * 1024;       // compressed bytes across CONTINUATIONs
    std::size_t max_body = 64u * 1024 * 1024;         // per response; over it the stream is cancelled

    // A server answering PING and SETTINGS costs it nothing and us an ACK
    // each; past this rate it is a flood and the connection is ended
    double control_burst = 200.0;
    double control_per_second = 50.0;
};

enum class h2_outcome : std::uint8_t {
    response,            // a whole response, trailers included
    refused,             // the server did not process it (REFUSED_STREAM or GOAWAY): safe to retry
    reset,               // the server reset the stream
    malformed,           // the response broke RFC 9113 §8; we reset the stream
    too_large,           // the body or the header list went over our limits; we cancelled the stream
    connection_failed,   // the connection failed or ended under it
};

inline const char* to_string(h2_outcome o) noexcept
{
    switch (o) {
    case h2_outcome::response: return "response";
    case h2_outcome::refused: return "refused by the server";
    case h2_outcome::reset: return "reset by the server";
    case h2_outcome::malformed: return "malformed response";
    case h2_outcome::too_large: return "response too large";
    case h2_outcome::connection_failed: return "connection failed";
    }
    return "?";
}

struct h2_completion {
    std::uint32_t stream = 0;
    h2_outcome outcome = h2_outcome::response;
    response res;                                      // the response, when outcome is `response`
    h2_error code = h2_error::no_error;                // the RST_STREAM or GOAWAY code, when there was one
    std::string detail;
    bool started = false;                              // any part of a response arrived
};

class h2_client_session {
public:
    explicit h2_client_session(h2_client_config cfg = {}) : cfg_(cfg)
    {
        dec_.initial_table_size(cfg_.header_table_size);
        conn_recv_window_ = std::int64_t(std::max<std::uint32_t>(cfg_.connection_window, 65535));
        ctl_bucket_.configure(cfg_.control_burst, cfg_.control_per_second);
    }

    // The connection preface: the magic octets, our SETTINGS, and the
    // connection window raised past its fixed 65535 start. Requests may
    // follow at once; §3.4 does not make a client wait for the server's
    // SETTINGS.
    void start()
    {
        if (started_)
            return;
        started_ = true;
        out_.append(kH2Preface, kH2PrefaceLen);
        const std::uint16_t ids[5] = {h2_settings_header_table_size, h2_settings_enable_push,
                                      h2_settings_initial_window_size, h2_settings_max_frame_size,
                                      h2_settings_max_header_list_size};
        const std::uint32_t vals[5] = {cfg_.header_table_size, 0u, cfg_.initial_window_size, cfg_.max_frame_size,
                                       cfg_.max_header_list_size};
        detail::h2_put_header(out_, 30, h2_frame::settings, 0, 0);
        for (int i = 0; i < 5; ++i) {
            out_.push_back(char((ids[i] >> 8) & 0xff));
            out_.push_back(char(ids[i] & 0xff));
            detail::h2_put32(out_, vals[i]);
        }
        if (conn_recv_window_ > 65535)
            detail::h2_put_window_update(out_, 0, std::uint32_t(conn_recv_window_ - 65535));
    }

    // Opens a stream carrying one request; its id, or 0 when this connection
    // will take no more (failed, GOAWAY received, at the server's concurrency
    // limit, or out of stream ids). Field names are lowercased; Host becomes
    // :authority and the connection-specific fields (§8.2.2) are dropped.
    std::uint32_t submit(const std::string& method, const std::string& scheme, const std::string& authority,
                         const std::string& path, const std::vector<std::pair<std::string, std::string>>& headers,
                         std::string body)
    {
        if (!can_open() || method.empty() || path.empty())
            return 0;
        const std::uint32_t sid = next_stream_;
        next_stream_ += 2;

        fields_out_.clear();
        fields_out_.emplace_back(":method", method);
        fields_out_.emplace_back(":scheme", scheme);
        fields_out_.emplace_back(":authority", authority);
        fields_out_.emplace_back(":path", path);
        bool have_length = false;
        std::string n;
        for (const auto& h : headers) {
            n = h.first;
            detail::ascii_lower(n);
            if (n.empty() || n == "host" || detail::h2_connection_specific(n))
                continue;
            if (n == "te" && h.second != "trailers")
                continue;
            if (n == "content-length")
                have_length = true;
            fields_out_.emplace_back(n, detail::h2_sanitise(h.second));
        }
        const bool body_method = method == "POST" || method == "PUT" || method == "PATCH";
        if (!have_length && (!body.empty() || body_method)) {
            std::string len;
            detail::append_uint(len, body.size());
            fields_out_.emplace_back("content-length", std::move(len));
        }
        std::string block;
        enc_.encode(fields_out_, block);

        stream& s = streams_[sid];
        s.id = sid;
        s.head_request = method == "HEAD";
        s.send_window = std::int64_t(peer_initial_window_);
        s.recv_window = std::int64_t(cfg_.initial_window_size);
        const bool end = body.empty();
        detail::h2_put_header_block(out_, block, peer_max_frame_, end, sid);
        if (end) {
            s.end_sent = true;
        } else {
            s.out = std::move(body);
            pump();
        }
        return sid;
    }

    // Frames from the server. Consumes every whole frame at the front of
    // `in`; a partial one stays for the next call.
    void consume(std::string& in)
    {
        if (failed_) {
            in.clear();
            return;
        }
        std::size_t off = 0;
        while (!failed_) {
            if (in.size() - off < 9)
                break;
            const unsigned char* p = reinterpret_cast<const unsigned char*>(in.data()) + off;
            const detail::h2_frame_header fh = detail::h2_read_header(p);
            if (fh.length > cfg_.max_frame_size) {
                connection_error(h2_error::frame_size_error, "frame exceeds SETTINGS_MAX_FRAME_SIZE");
                break;
            }
            if (in.size() - off < 9u + fh.length)
                break;
            if (!first_frame_seen_) {
                // §3.4: the server's preface is a SETTINGS frame, first
                first_frame_seen_ = true;
                if (fh.type != std::uint8_t(h2_frame::settings) || (fh.flags & h2_flag_ack)) {
                    connection_error(h2_error::protocol_error, "the server preface must be SETTINGS");
                    break;
                }
            }
            if (cont_stream_ && (fh.type != std::uint8_t(h2_frame::continuation) || fh.stream != cont_stream_)) {
                connection_error(h2_error::protocol_error, "frame interleaved with a header block");
                break;
            }
            handle_frame(fh, p + 9);
            off += 9u + fh.length;
        }
        if (failed_) {
            in.clear();
            return;
        }
        if (off)
            in.erase(0, off);
        flush_window_updates();
        pump();
    }

    // Stop a stream we no longer want (a timeout, say): RST_STREAM CANCEL,
    // and no completion. Frames already in flight for it are absorbed.
    void cancel(std::uint32_t sid)
    {
        auto it = streams_.find(sid);
        if (it == streams_.end() || failed_)
            return;
        detail::h2_put_rst(out_, sid, h2_error::cancel);
        streams_.erase(it);
    }

    // The transport is gone: every open stream completes as
    // connection_failed. A stream above a GOAWAY's last id has already
    // completed as refused.
    void connection_lost(const std::string& why)
    {
        fail_all(h2_outcome::connection_failed, h2_error::no_error, why);
        failed_ = true;
    }

    // A graceful end: GOAWAY with NO_ERROR, after which nothing new opens
    void shutdown()
    {
        if (failed_ || goaway_sent_)
            return;
        goaway_sent_ = true;
        detail::h2_put_goaway(out_, 0, h2_error::no_error, nullptr);
    }

    // Bytes to write, in order. Take them after every call above.
    std::string take_output()
    {
        std::string o;
        o.swap(out_);
        return o;
    }
    bool has_output() const noexcept { return !out_.empty(); }

    std::vector<h2_completion> take_completions()
    {
        std::vector<h2_completion> c;
        c.swap(done_);
        return c;
    }

    bool can_open() const noexcept
    {
        return started_ && !failed_ && !goaway_received_ && !goaway_sent_ && next_stream_ <= 0x7fffffffu &&
               streams_.size() < peer_max_concurrent_;
    }
    bool failed() const noexcept { return failed_; }
    bool goaway_received() const noexcept { return goaway_received_; }
    // Nothing open and nothing new will be: the connection can be closed
    bool finished() const noexcept { return failed_ || ((goaway_received_ || goaway_sent_) && streams_.empty()); }
    std::size_t active_streams() const noexcept { return streams_.size(); }
    std::uint32_t peer_max_concurrent_streams() const noexcept { return peer_max_concurrent_; }
    h2_error last_error() const noexcept { return last_error_; }
    const std::string& last_error_detail() const noexcept { return last_detail_; }
    // Request body bytes the server's windows have not taken yet
    std::size_t buffered_bytes() const noexcept
    {
        std::size_t n = 0;
        for (const auto& kv : streams_)
            n += kv.second.out.size() - kv.second.out_off;
        return n;
    }

private:
    struct stream {
        std::uint32_t id = 0;
        bool head_request = false;
        std::string out;                 // request body not yet sent
        std::size_t out_off = 0;
        bool end_sent = false;
        std::int64_t send_window = 65535;
        std::int64_t recv_window = 65535;
        std::uint32_t pending_update = 0;
        response res;
        bool have_head = false;
        bool started = false;
        bool has_length = false;
        unsigned long long length = 0;
    };

    // ------------------------------------------------------------ errors

    void complete(std::uint32_t sid, h2_outcome o, h2_error code, std::string why)
    {
        auto it = streams_.find(sid);
        if (it == streams_.end())
            return;
        h2_completion c;
        c.stream = sid;
        c.outcome = o;
        c.code = code;
        c.detail = std::move(why);
        c.started = it->second.started;
        if (o == h2_outcome::response)
            c.res = std::move(it->second.res);
        streams_.erase(it);
        done_.push_back(std::move(c));
    }

    // One exchange fails; the connection and its other streams carry on
    void stream_error(std::uint32_t sid, h2_error rst, h2_outcome o, std::string why)
    {
        detail::h2_put_rst(out_, sid, rst);
        complete(sid, o, rst, std::move(why));
    }

    void fail_all(h2_outcome o, h2_error code, const std::string& why)
    {
        std::vector<std::uint32_t> ids;
        ids.reserve(streams_.size());
        for (const auto& kv : streams_)
            ids.push_back(kv.first);
        for (std::uint32_t sid : ids)
            complete(sid, o, code, why);
    }

    void connection_error(h2_error code, const char* why)
    {
        if (failed_)
            return;
        last_error_ = code;
        last_detail_ = why;
        if (!goaway_sent_) {
            goaway_sent_ = true;
            detail::h2_put_goaway(out_, 0, code, why);   // we accept no streams, so the last is 0
        }
        fail_all(h2_outcome::connection_failed, code, why);
        failed_ = true;
    }

    stream* find(std::uint32_t sid) noexcept
    {
        auto it = streams_.find(sid);
        return it == streams_.end() ? nullptr : &it->second;
    }

    // A stream id we opened at some point, now gone or not
    bool opened_by_us(std::uint32_t sid) const noexcept { return (sid & 1u) && sid < next_stream_; }

    // ------------------------------------------------------------ windows

    void flush_window_updates()
    {
        if (conn_pending_update_) {
            detail::h2_put_window_update(out_, 0, conn_pending_update_);
            conn_recv_window_ += std::int64_t(conn_pending_update_);
            conn_pending_update_ = 0;
        }
        for (auto& kv : streams_) {
            stream& s = kv.second;
            if (s.pending_update) {
                detail::h2_put_window_update(out_, s.id, s.pending_update);
                s.recv_window += std::int64_t(s.pending_update);
                s.pending_update = 0;
            }
        }
    }

    // Request bodies, as far as the server's windows allow
    void pump()
    {
        if (failed_)
            return;
        for (auto& kv : streams_) {
            stream& s = kv.second;
            while (!s.end_sent) {
                const std::size_t left = s.out.size() - s.out_off;
                const std::int64_t window = std::min(conn_send_window_, s.send_window);
                if (left > 0 && window <= 0)
                    break;                       // a WINDOW_UPDATE restarts us
                std::size_t n = std::min<std::size_t>(left, std::size_t(std::max<std::int64_t>(window, 0)));
                n = std::min<std::size_t>(n, peer_max_frame_);
                const bool last = n == left;
                detail::h2_put_header(out_, std::uint32_t(n), h2_frame::data, last ? h2_flag_end_stream : 0, s.id);
                out_.append(s.out, s.out_off, n);
                s.out_off += n;
                conn_send_window_ -= std::int64_t(n);
                s.send_window -= std::int64_t(n);
                if (last) {
                    s.end_sent = true;
                    std::string().swap(s.out);
                    s.out_off = 0;
                }
            }
        }
    }

    // ------------------------------------------------------------ frames

    void handle_frame(const detail::h2_frame_header& fh, const unsigned char* p)
    {
        switch (fh.type) {
        case std::uint8_t(h2_frame::data): on_data(fh, p); break;
        case std::uint8_t(h2_frame::headers): on_headers(fh, p); break;
        case std::uint8_t(h2_frame::priority):
            if (fh.stream == 0)
                connection_error(h2_error::protocol_error, "PRIORITY on stream 0");
            else if (fh.length != 5)
                connection_error(h2_error::frame_size_error, "PRIORITY length is not 5");
            break;                               // deprecated by RFC 9113; ignored
        case std::uint8_t(h2_frame::rst_stream): on_rst(fh, p); break;
        case std::uint8_t(h2_frame::settings): on_settings(fh, p); break;
        case std::uint8_t(h2_frame::push_promise):
            connection_error(h2_error::protocol_error, "PUSH_PROMISE with push disabled");
            break;
        case std::uint8_t(h2_frame::ping): on_ping(fh, p); break;
        case std::uint8_t(h2_frame::goaway): on_goaway(fh, p); break;
        case std::uint8_t(h2_frame::window_update): on_window_update(fh, p); break;
        case std::uint8_t(h2_frame::continuation): on_continuation(fh, p); break;
        default:
            break;                               // §5.5: unknown types are ignored
        }
    }

    // Strips padding: false (after a connection error) when it does not fit
    bool unpad(const detail::h2_frame_header& fh, const unsigned char* p, std::uint32_t& off, std::uint32_t& pad,
               const char* what)
    {
        off = 0;
        pad = 0;
        if (fh.flags & h2_flag_padded) {
            if (fh.length < 1) {
                connection_error(h2_error::protocol_error, what);
                return false;
            }
            pad = p[0];
            off = 1;
        }
        return true;
    }

    void on_data(const detail::h2_frame_header& fh, const unsigned char* p)
    {
        const std::uint32_t sid = fh.stream;
        if (sid == 0)
            return connection_error(h2_error::protocol_error, "DATA on stream 0");
        // The window is spent whether or not we want the stream
        conn_recv_window_ -= std::int64_t(fh.length);
        conn_pending_update_ += fh.length;
        if (conn_recv_window_ < 0)
            return connection_error(h2_error::flow_control_error, "connection receive window exceeded");
        std::uint32_t off = 0, pad = 0;
        if (!unpad(fh, p, off, pad, "padded DATA with no pad length"))
            return;
        if (std::uint32_t(pad) + off > fh.length)
            return connection_error(h2_error::protocol_error, "DATA padding exceeds the frame");

        stream* s = find(sid);
        if (!s) {
            if (!opened_by_us(sid))
                connection_error(h2_error::protocol_error, "DATA on an idle stream");
            return;                              // a stream we finished or cancelled: absorbed
        }
        s->recv_window -= std::int64_t(fh.length);
        if (s->recv_window < 0)
            return stream_error(sid, h2_error::flow_control_error, h2_outcome::malformed,
                                "stream receive window exceeded");
        s->pending_update += fh.length;
        if (!s->have_head)
            return stream_error(sid, h2_error::protocol_error, h2_outcome::malformed, "DATA before the response head");
        const std::uint32_t n = fh.length - off - pad;
        if (s->res.body.size() + n > cfg_.max_body)
            return stream_error(sid, h2_error::cancel, h2_outcome::too_large, "response body over max_body");
        s->res.body.append(reinterpret_cast<const char*>(p + off), n);
        if (fh.flags & h2_flag_end_stream)
            end_of_response(*s);
    }

    void on_headers(const detail::h2_frame_header& fh, const unsigned char* p)
    {
        const std::uint32_t sid = fh.stream;
        if (sid == 0)
            return connection_error(h2_error::protocol_error, "HEADERS on stream 0");
        if (!opened_by_us(sid))
            return connection_error(h2_error::protocol_error, "HEADERS on a stream the client did not open");
        std::uint32_t off = 0, pad = 0;
        if (!unpad(fh, p, off, pad, "padded HEADERS with no pad length"))
            return;
        if (fh.flags & h2_flag_priority) {
            if (fh.length < off + 5u)
                return connection_error(h2_error::frame_size_error, "HEADERS priority block truncated");
            off += 5;
        }
        if (off + pad > fh.length)
            return connection_error(h2_error::protocol_error, "HEADERS padding exceeds the frame");
        if (fh.length - off - pad > cfg_.max_header_block)
            return connection_error(h2_error::enhance_your_calm, "header block too large");
        block_.assign(reinterpret_cast<const char*>(p + off), fh.length - off - pad);
        block_stream_ = sid;
        block_end_stream_ = (fh.flags & h2_flag_end_stream) != 0;
        if (fh.flags & h2_flag_end_headers) {
            cont_stream_ = 0;
            finish_header_block();
        } else {
            cont_stream_ = sid;
        }
    }

    void on_continuation(const detail::h2_frame_header& fh, const unsigned char* p)
    {
        if (cont_stream_ == 0 || fh.stream != cont_stream_)
            return connection_error(h2_error::protocol_error, "CONTINUATION without HEADERS");
        if (block_.size() + fh.length > cfg_.max_header_block)
            return connection_error(h2_error::enhance_your_calm, "header block too large");
        block_.append(reinterpret_cast<const char*>(p), fh.length);
        if (fh.flags & h2_flag_end_headers) {
            cont_stream_ = 0;
            finish_header_block();
        }
    }

    void finish_header_block()
    {
        const std::uint32_t sid = block_stream_;
        fields_.clear();
        const unsigned char* b = reinterpret_cast<const unsigned char*>(block_.data());
        const hpack::status st =
            dec_.decode(b, b + block_.size(), fields_, cfg_.max_header_list_size, cfg_.max_header_fields);
        block_.clear();
        block_stream_ = 0;
        if (st == hpack::status::compression_error)
            return connection_error(h2_error::compression_error, "HPACK decoding failed");

        stream* s = find(sid);
        if (!s)
            return;                              // decoded for the table's sake; nobody wants it
        s->started = true;
        if (st == hpack::status::list_too_large)
            return stream_error(sid, h2_error::cancel, h2_outcome::too_large, "response header list too large");

        if (s->have_head) {
            // Trailers: the last thing on the stream, and never pseudo-fields
            if (!block_end_stream_)
                return stream_error(sid, h2_error::protocol_error, h2_outcome::malformed,
                                    "a second header block without END_STREAM");
            for (const auto& f : fields_) {
                if (!f.first.empty() && f.first[0] == ':')
                    return stream_error(sid, h2_error::protocol_error, h2_outcome::malformed,
                                        "pseudo-header in trailers");
                if (!detail::h2_valid_name(f.first) || !detail::h2_valid_value(f.second) ||
                    detail::h2_connection_specific(f.first))
                    return stream_error(sid, h2_error::protocol_error, h2_outcome::malformed, "bad trailer field");
                s->res.trailers.add(f.first, f.second);
            }
            return end_of_response(*s);
        }

        const char* why = nullptr;
        int status = 0;
        if (!read_head(*s, status, why))
            return stream_error(sid, h2_error::protocol_error, h2_outcome::malformed, why);
        if (status >= 100 && status < 200) {
            // Interim (100 Continue, 103 Early Hints): the real head follows.
            // A 1xx that ends the stream leaves no final response at all.
            if (block_end_stream_)
                return stream_error(sid, h2_error::protocol_error, h2_outcome::malformed,
                                    "an interim response ended the stream");
            s->res.headers.clear();
            s->has_length = false;
            return;
        }
        s->have_head = true;
        if (block_end_stream_)
            end_of_response(*s);
    }

    // §8.3.2 and §8.2: a response head is :status and nothing else pseudo,
    // then lowercase, well-formed fields with no connection-specific ones
    bool read_head(stream& s, int& status, const char*& why)
    {
        bool have_status = false, seen_regular = false;
        response& r = s.res;
        r.headers.clear();
        for (const auto& f : fields_) {
            const std::string& n = f.first;
            const std::string& v = f.second;
            if (n.empty() || !detail::h2_valid_value(v)) {
                why = "bad field";
                return false;
            }
            if (n[0] == ':') {
                if (seen_regular || n != ":status" || have_status) {
                    why = "bad pseudo-header in a response";
                    return false;
                }
                if (v.size() != 3 || !detail::is_digit(v[0]) || !detail::is_digit(v[1]) || !detail::is_digit(v[2])) {
                    why = "bad :status";
                    return false;
                }
                status = (v[0] - '0') * 100 + (v[1] - '0') * 10 + (v[2] - '0');
                if (status < 100) {
                    why = "bad :status";
                    return false;
                }
                have_status = true;
                continue;
            }
            seen_regular = true;
            if (!detail::h2_valid_name(n) || detail::h2_connection_specific(n)) {
                why = "bad field name";
                return false;
            }
            if (n == "content-length") {
                unsigned long long declared = 0;
                if (!detail::h2_parse_uint(v, declared) || (s.has_length && s.length != declared)) {
                    why = "bad content-length";
                    return false;
                }
                s.has_length = true;
                s.length = declared;
            }
            r.headers.add(n, v);
        }
        if (!have_status) {
            why = "no :status";
            return false;
        }
        if (status == 101) {
            why = "101 is not an HTTP/2 response";   // §8.6
            return false;
        }
        r.status = status;
        r.http_major = 2;
        r.http_minor = 0;
        r.keep_alive = true;
        r.reason.clear();
        return true;
    }

    void end_of_response(stream& s)
    {
        if (!s.have_head)
            return stream_error(s.id, h2_error::protocol_error, h2_outcome::malformed, "the stream ended without a response");
        // §8.1.1: a body that disagrees with its Content-Length is malformed;
        // HEAD, 204 and 304 carry the length of a body that is not sent
        const int st = s.res.status;
        if (s.has_length && !s.head_request && st != 204 && st != 304 && s.length != s.res.body.size())
            return stream_error(s.id, h2_error::protocol_error, h2_outcome::malformed,
                                "body length disagrees with content-length");
        // §8.1: a server may answer before the request body is done; the
        // rest of it is not wanted, and saying so frees the stream at once
        if (!s.end_sent)
            detail::h2_put_rst(out_, s.id, h2_error::no_error);
        complete(s.id, h2_outcome::response, h2_error::no_error, {});
    }

    void on_rst(const detail::h2_frame_header& fh, const unsigned char* p)
    {
        if (fh.stream == 0)
            return connection_error(h2_error::protocol_error, "RST_STREAM on stream 0");
        if (fh.length != 4)
            return connection_error(h2_error::frame_size_error, "RST_STREAM length is not 4");
        if (!find(fh.stream)) {
            if (!opened_by_us(fh.stream))
                connection_error(h2_error::protocol_error, "RST_STREAM on an idle stream");
            return;
        }
        const h2_error code = h2_error(detail::h2_get32(p));
        // A response the server has finished with NO_ERROR is a server that
        // did not want the rest of our body; anything else is a failure
        complete(fh.stream, code == h2_error::refused_stream ? h2_outcome::refused : h2_outcome::reset, code,
                 "RST_STREAM");
    }

    void on_settings(const detail::h2_frame_header& fh, const unsigned char* p)
    {
        if (fh.stream != 0)
            return connection_error(h2_error::protocol_error, "SETTINGS on a non-zero stream");
        if (fh.flags & h2_flag_ack) {
            if (fh.length != 0)
                connection_error(h2_error::frame_size_error, "SETTINGS ACK with a payload");
            return;
        }
        if (fh.length % 6 != 0)
            return connection_error(h2_error::frame_size_error, "SETTINGS length is not a multiple of 6");
        if (!ctl_bucket_.take(std::chrono::steady_clock::now()))
            return connection_error(h2_error::enhance_your_calm, "SETTINGS flood");
        for (std::uint32_t i = 0; i + 6 <= fh.length; i += 6) {
            const std::uint16_t id = std::uint16_t((std::uint32_t(p[i]) << 8) | p[i + 1]);
            const std::uint32_t v = detail::h2_get32(p + i + 2);
            switch (id) {
            case h2_settings_header_table_size:
                enc_.peer_table_size(v);
                break;
            case h2_settings_enable_push:
                // §6.5.2: only a client may set it, so a server sending
                // anything but 0 is an error
                if (v != 0)
                    return connection_error(h2_error::protocol_error, "SETTINGS_ENABLE_PUSH from a server");
                break;
            case h2_settings_max_concurrent_streams:
                peer_max_concurrent_ = v;
                break;
            case h2_settings_initial_window_size: {
                if (v > 0x7fffffffu)
                    return connection_error(h2_error::flow_control_error, "SETTINGS_INITIAL_WINDOW_SIZE above 2^31-1");
                // §6.9.2: a delta on every open stream, not a reset
                const std::int64_t delta = std::int64_t(v) - std::int64_t(peer_initial_window_);
                peer_initial_window_ = v;
                for (auto& kv : streams_) {
                    kv.second.send_window += delta;
                    if (kv.second.send_window > 0x7fffffffLL)
                        return connection_error(h2_error::flow_control_error, "stream send window overflowed");
                }
                break;
            }
            case h2_settings_max_frame_size:
                if (v < 16384u || v > 16777215u)
                    return connection_error(h2_error::protocol_error, "SETTINGS_MAX_FRAME_SIZE out of range");
                peer_max_frame_ = v;
                break;
            default:
                break;                           // MAX_HEADER_LIST_SIZE is advisory; unknown ids are ignored
            }
        }
        detail::h2_put_header(out_, 0, h2_frame::settings, h2_flag_ack, 0);
    }

    void on_ping(const detail::h2_frame_header& fh, const unsigned char* p)
    {
        if (fh.stream != 0)
            return connection_error(h2_error::protocol_error, "PING on a non-zero stream");
        if (fh.length != 8)
            return connection_error(h2_error::frame_size_error, "PING length is not 8");
        if (fh.flags & h2_flag_ack)
            return;
        if (!ctl_bucket_.take(std::chrono::steady_clock::now()))
            return connection_error(h2_error::enhance_your_calm, "PING flood");
        detail::h2_put_header(out_, 8, h2_frame::ping, h2_flag_ack, 0);
        out_.append(reinterpret_cast<const char*>(p), 8);
    }

    void on_goaway(const detail::h2_frame_header& fh, const unsigned char* p)
    {
        if (fh.stream != 0)
            return connection_error(h2_error::protocol_error, "GOAWAY on a non-zero stream");
        if (fh.length < 8)
            return connection_error(h2_error::frame_size_error, "GOAWAY shorter than 8 octets");
        const std::uint32_t last = detail::h2_get32(p) & 0x7fffffffu;
        const h2_error code = h2_error(detail::h2_get32(p + 4));
        goaway_received_ = true;
        last_error_ = code;
        last_detail_.assign(reinterpret_cast<const char*>(p + 8), fh.length - 8);
        // §6.8: streams above `last` were never processed - safe to retry
        // anywhere. Those at or below it may finish; a second GOAWAY can only
        // lower `last`, so the same rule applies again.
        std::vector<std::uint32_t> unprocessed;
        for (const auto& kv : streams_)
            if (kv.first > last)
                unprocessed.push_back(kv.first);
        for (std::uint32_t sid : unprocessed)
            complete(sid, h2_outcome::refused, code, "GOAWAY");
    }

    void on_window_update(const detail::h2_frame_header& fh, const unsigned char* p)
    {
        if (fh.length != 4)
            return connection_error(h2_error::frame_size_error, "WINDOW_UPDATE length is not 4");
        const std::uint32_t inc = detail::h2_get32(p) & 0x7fffffffu;
        if (fh.stream == 0) {
            if (inc == 0)
                return connection_error(h2_error::protocol_error, "WINDOW_UPDATE increment of 0");
            conn_send_window_ += std::int64_t(inc);
            if (conn_send_window_ > 0x7fffffffLL)
                return connection_error(h2_error::flow_control_error, "connection send window overflowed");
            return;
        }
        stream* s = find(fh.stream);
        if (!s) {
            if (!opened_by_us(fh.stream))
                connection_error(h2_error::protocol_error, "WINDOW_UPDATE on an idle stream");
            return;
        }
        if (inc == 0)
            return stream_error(fh.stream, h2_error::protocol_error, h2_outcome::malformed, "WINDOW_UPDATE of 0");
        s->send_window += std::int64_t(inc);
        if (s->send_window > 0x7fffffffLL)
            return stream_error(fh.stream, h2_error::flow_control_error, h2_outcome::malformed,
                                "stream send window overflowed");
    }

    // ------------------------------------------------------------ state

    h2_client_config cfg_;
    hpack::decoder dec_;
    hpack::encoder enc_;
    std::vector<hpack::field> fields_;
    std::vector<hpack::field> fields_out_;

    std::map<std::uint32_t, stream> streams_;
    std::uint32_t next_stream_ = 1;

    std::string block_;
    std::uint32_t block_stream_ = 0;
    std::uint32_t cont_stream_ = 0;
    bool block_end_stream_ = false;

    // Until the server's SETTINGS say otherwise: RFC 9113's defaults, and a
    // concurrency limit of 100 rather than "unlimited", which §6.5.2
    // recommends servers not go below
    std::uint32_t peer_max_frame_ = 16384;
    std::uint32_t peer_initial_window_ = 65535;
    std::uint32_t peer_max_concurrent_ = 100;

    std::int64_t conn_send_window_ = 65535;
    std::int64_t conn_recv_window_ = 65535;
    std::uint32_t conn_pending_update_ = 0;

    detail::h2_bucket ctl_bucket_;

    std::string out_;
    std::vector<h2_completion> done_;
    h2_error last_error_ = h2_error::no_error;
    std::string last_detail_;
    bool started_ = false;
    bool first_frame_seen_ = false;
    bool goaway_received_ = false;
    bool goaway_sent_ = false;
    bool failed_ = false;
};

} // namespace http
} // namespace snicholls

#endif // ts_moveables_http_h2_client_hpp
