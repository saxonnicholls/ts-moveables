//
//  http/parser.hpp
//  TSMoveables
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  The HTTP/1.1 request parser: incremental, resumable, and deliberately
//  strict.
//
//  Bytes may arrive one at a time and no split changes the outcome. Content-
//  Length with Transfer-Encoding, conflicting duplicate Content-Length,
//  whitespace before a colon, obsolete line folding and bare-LF line endings
//  are all rejected - every one is a documented request-smuggling vector, and
//  leniency is how a server becomes a gadget in someone else's attack chain.
//
//  No IO here either: hand it a buffer, ask what it made of it.
//

#ifndef http_parser_hpp
#define http_parser_hpp

#include "message.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace snicholls {
namespace http {

// ---------------------------------------------------------------- the parser
//
// Incremental and resumable: bytes may arrive one at a time, and no input
// split changes the outcome. Deliberately strict about framing - see the
// header comment for why leniency is a security position, not a kindness.

struct parse_limits {
    std::size_t max_request_line = 8u * 1024;
    std::size_t max_header_bytes = 32u * 1024;
    std::size_t max_headers      = 128;
    std::size_t max_body         = 8u * 1024 * 1024;
    std::size_t max_chunk_line   = 1024;
};

class request_parser {
public:
    enum class status { need_more, have_request, failed };

    request_parser() = default;
    explicit request_parser(parse_limits lim) : lim_(lim) {}

    void limits(const parse_limits& l) noexcept { lim_ = l; }
    const parse_limits& limits() const noexcept { return lim_; }

    request& message() noexcept { return req_; }
    const request& message() const noexcept { return req_; }

    bool headers_done() const noexcept { return headers_done_; }
    bool expects_continue() const noexcept { return expect_continue_; }
    bool started() const noexcept { return started_; }
    int error_status() const noexcept { return err_status_; }
    const char* error_reason() const noexcept { return err_reason_ ? err_reason_ : ""; }

    void reset()
    {
        req_.clear();                           // reuse the buffers, do not free them
        st_ = state::start_line;
        headers_done_ = expect_continue_ = started_ = false;
        header_bytes_ = 0;
        n_headers_ = 0;
        body_remaining_ = 0;
        chunk_remaining_ = 0;
        framing_ = framing::none;
        err_status_ = 0;
        err_reason_ = nullptr;
    }

    status parse(const char* data, std::size_t len, std::size_t& consumed)
    {
        consumed = 0;
        if (st_ == state::failed)
            return status::failed;
        if (st_ == state::complete)
            return status::have_request;
        if (len == 0)
            return status::need_more;
        started_ = true;

        const char* p = data;
        const char* const end = data + len;

        for (;;) {
            switch (st_) {

            case state::start_line: {
                const char* lf = find_lf(p, end);
                if (!lf) {
                    if (std::size_t(end - p) > lim_.max_request_line)
                        return fail(414, "request line too long");
                    consumed = std::size_t(p - data);
                    return status::need_more;
                }
                if (std::size_t(lf - p) + 1 > lim_.max_request_line)
                    return fail(414, "request line too long");
                const char* line_end = nullptr;
                if (!terminate_line(p, lf, line_end))
                    return fail(400, "bare LF or stray CR in request line");
                if (line_end == p) {            // RFC 9112: ignore a leading empty line
                    p = lf + 1;
                    break;
                }
                if (!parse_request_line(p, line_end))
                    return status::failed;
                p = lf + 1;
                st_ = state::headers;
                break;
            }

            case state::headers: {
                const char* lf = find_lf(p, end);
                if (!lf) {
                    if (header_bytes_ + std::size_t(end - p) > lim_.max_header_bytes)
                        return fail(431, "header block too large");
                    consumed = std::size_t(p - data);
                    return status::need_more;
                }
                header_bytes_ += std::size_t(lf - p) + 1;
                if (header_bytes_ > lim_.max_header_bytes)
                    return fail(431, "header block too large");
                const char* line_end = nullptr;
                if (!terminate_line(p, lf, line_end))
                    return fail(400, "bare LF or stray CR in header field");
                if (line_end == p) {            // blank line: end of the header block
                    p = lf + 1;
                    if (!finish_headers())
                        return status::failed;
                    headers_done_ = true;
                    if (framing_ == framing::none) {
                        consumed = std::size_t(p - data);
                        return complete();
                    }
                    st_ = (framing_ == framing::identity) ? state::body_identity
                                                          : state::chunk_size;
                    break;
                }
                if (*p == ' ' || *p == '\t')
                    return fail(400, "obsolete line folding");
                if (++n_headers_ > lim_.max_headers)
                    return fail(431, "too many header fields");
                if (!parse_header_line(p, line_end))
                    return status::failed;
                p = lf + 1;
                break;
            }

            case state::body_identity: {
                const std::size_t take = std::min(std::size_t(end - p), body_remaining_);
                req_.body.append(p, take);
                p += take;
                body_remaining_ -= take;
                consumed = std::size_t(p - data);
                if (body_remaining_ == 0)
                    return complete();
                return status::need_more;
            }

            case state::chunk_size: {
                const char* lf = find_lf(p, end);
                if (!lf) {
                    if (std::size_t(end - p) > lim_.max_chunk_line)
                        return fail(400, "chunk size line too long");
                    consumed = std::size_t(p - data);
                    return status::need_more;
                }
                if (std::size_t(lf - p) + 1 > lim_.max_chunk_line)
                    return fail(400, "chunk size line too long");
                const char* line_end = nullptr;
                if (!terminate_line(p, lf, line_end))
                    return fail(400, "bare LF or stray CR in chunk header");
                std::size_t size = 0;
                if (!parse_chunk_size(p, line_end, size))
                    return status::failed;
                p = lf + 1;
                if (size == 0) {
                    st_ = state::trailers;
                    break;
                }
                if (req_.body.size() + size > lim_.max_body)
                    return fail(413, "chunked body too large");
                chunk_remaining_ = size;
                st_ = state::chunk_data;
                break;
            }

            case state::chunk_data: {
                const std::size_t take = std::min(std::size_t(end - p), chunk_remaining_);
                req_.body.append(p, take);
                p += take;
                chunk_remaining_ -= take;
                if (chunk_remaining_ != 0) {
                    consumed = std::size_t(p - data);
                    return status::need_more;
                }
                st_ = state::chunk_crlf;
                break;
            }

            case state::chunk_crlf: {
                if (end - p < 2) {
                    consumed = std::size_t(p - data);
                    return status::need_more;
                }
                if (p[0] != '\r' || p[1] != '\n')
                    return fail(400, "malformed chunk terminator");
                p += 2;
                st_ = state::chunk_size;
                break;
            }

            case state::trailers: {
                const char* lf = find_lf(p, end);
                if (!lf) {
                    if (header_bytes_ + std::size_t(end - p) > lim_.max_header_bytes)
                        return fail(431, "trailer block too large");
                    consumed = std::size_t(p - data);
                    return status::need_more;
                }
                header_bytes_ += std::size_t(lf - p) + 1;
                if (header_bytes_ > lim_.max_header_bytes)
                    return fail(431, "trailer block too large");
                const char* line_end = nullptr;
                if (!terminate_line(p, lf, line_end))
                    return fail(400, "bare LF or stray CR in trailer");
                const bool blank = (line_end == p);
                p = lf + 1;
                if (blank) {                    // blank line: trailers done
                    consumed = std::size_t(p - data);
                    return complete();
                }
                // Trailers are validated and discarded: merging them into the
                // header set is a header-injection gadget
                break;
            }

            case state::complete:
                consumed = std::size_t(p - data);
                return status::have_request;

            case state::failed:
                return status::failed;
            }
        }
    }

private:
    enum class state {
        start_line, headers, body_identity,
        chunk_size, chunk_data, chunk_crlf, trailers,
        complete, failed
    };
    enum class framing { none, identity, chunked };

    static const char* find_lf(const char* p, const char* end) noexcept
    {
        return static_cast<const char*>(std::memchr(p, '\n', std::size_t(end - p)));
    }

    // A line must end CRLF, and must not contain a stray CR: bare LF framing
    // is the oldest request-smuggling trick there is
    static bool terminate_line(const char* begin, const char* lf, const char*& line_end) noexcept
    {
        if (lf == begin || *(lf - 1) != '\r')
            return false;
        line_end = lf - 1;
        return std::memchr(begin, '\r', std::size_t(line_end - begin)) == nullptr;
    }

    status fail(int code, const char* why) noexcept
    {
        st_ = state::failed;
        err_status_ = code;
        err_reason_ = why;
        return status::failed;
    }

    bool bad(int code, const char* why) noexcept
    {
        fail(code, why);
        return false;
    }

    status complete() noexcept
    {
        st_ = state::complete;
        return status::have_request;
    }

    bool parse_request_line(const char* b, const char* e)
    {
        const char* sp1 = static_cast<const char*>(std::memchr(b, ' ', std::size_t(e - b)));
        if (!sp1 || sp1 == b)
            return bad(400, "malformed request line");
        const char* sp2 = static_cast<const char*>(
            std::memchr(sp1 + 1, ' ', std::size_t(e - (sp1 + 1))));
        if (!sp2 || sp2 == sp1 + 1)
            return bad(400, "malformed request line");
        if (std::memchr(sp2 + 1, ' ', std::size_t(e - (sp2 + 1))) != nullptr)
            return bad(400, "too many fields in request line");

        for (const char* q = b; q < sp1; ++q)
            if (!detail::is_token_char(*q))
                return bad(400, "invalid character in method");
        req_.method_text.assign(b, std::size_t(sp1 - b));
        req_.method = parse_method(req_.method_text);

        const char* v = sp2 + 1;
        if (std::size_t(e - v) != 8 || std::memcmp(v, "HTTP/", 5) != 0 ||
            !detail::is_digit(v[5]) || v[6] != '.' || !detail::is_digit(v[7]))
            return bad(400, "malformed HTTP version");
        req_.http_major = v[5] - '0';
        req_.http_minor = v[7] - '0';
        if (req_.http_major != 1)
            return bad(505, "unsupported HTTP version");

        return parse_target(sp1 + 1, sp2);
    }

    bool parse_target(const char* b, const char* e)
    {
        req_.target.assign(b, std::size_t(e - b));
        if (b == e)
            return bad(400, "empty request target");

        if (e - b == 1 && *b == '*') {          // asterisk-form (OPTIONS *)
            req_.path = "*";
            return true;
        }
        // Work over the incoming bytes rather than a copy of them
        const char* tb = b;
        if (*tb != '/') {                       // absolute-form: skip scheme://authority
            const char* scheme = static_cast<const char*>(
                detail::find_bytes(tb, std::size_t(e - tb), "://", 3));
            if (!scheme)
                return bad(400, "unsupported request-target form");
            const char* slash = static_cast<const char*>(
                std::memchr(scheme + 3, '/', std::size_t(e - (scheme + 3))));
            tb = slash ? slash : nullptr;
            absolute_form_ = true;
            if (!tb) {                          // "http://host" with no path
                req_.path = "/";
                req_.query.clear();
                return true;
            }
        }
        const char* q = static_cast<const char*>(std::memchr(tb, '?', std::size_t(e - tb)));
        const std::size_t path_len = std::size_t((q ? q : e) - tb);
        if (q)
            req_.query.assign(q + 1, std::size_t(e - (q + 1)));
        else
            req_.query.clear();
        if (!detail::percent_decode(tb, path_len, req_.path, false))
            return bad(400, "invalid percent-encoding in request target");
        if (req_.path.find('\0') != std::string::npos)
            return bad(400, "NUL byte in request target");
        return true;
    }

    bool parse_header_line(const char* b, const char* e)
    {
        const char* colon = static_cast<const char*>(std::memchr(b, ':', std::size_t(e - b)));
        if (!colon || colon == b)
            return bad(400, "malformed header field");
        // Any non-token character before the colon - notably a space - is a
        // hard 400: "Content-Length : 5" is a classic smuggling desync
        for (const char* q = b; q < colon; ++q)
            if (!detail::is_token_char(*q))
                return bad(400, "invalid character in field name");

        const char* vb = colon + 1;
        while (vb < e && (*vb == ' ' || *vb == '\t')) ++vb;
        const char* ve = e;
        while (ve > vb && (*(ve - 1) == ' ' || *(ve - 1) == '\t')) --ve;
        for (const char* q = vb; q < ve; ++q) {
            const unsigned char c = static_cast<unsigned char>(*q);
            if ((c < 0x20 && c != '\t') || c == 0x7f)
                return bad(400, "control character in field value");
        }

        std::string name(b, std::size_t(colon - b));
        detail::ascii_lower(name);              // lowercase, as HTTP/2 will require anyway
        req_.headers.add(std::move(name), std::string(vb, std::size_t(ve - vb)));
        return true;
    }

    // A Content-Length field may be a list of identical values; anything else
    // is a desync waiting to happen
    static bool parse_content_length(const std::string& v, unsigned long long& out)
    {
        bool any = false;
        std::size_t i = 0;
        while (i <= v.size()) {
            std::size_t j = v.find(',', i);
            if (j == std::string::npos)
                j = v.size();
            std::size_t b = i, e = j;
            while (b < e && (v[b] == ' ' || v[b] == '\t')) ++b;
            while (e > b && (v[e - 1] == ' ' || v[e - 1] == '\t')) --e;
            if (b == e)
                return false;
            unsigned long long n = 0;
            for (std::size_t k = b; k < e; ++k) {
                if (!detail::is_digit(v[k]))
                    return false;
                if (n > (~0ull - 9) / 10)
                    return false;
                n = n * 10 + unsigned(v[k] - '0');
            }
            if (any && n != out)
                return false;
            out = n;
            any = true;
            i = j + 1;
        }
        return any;
    }

    bool finish_headers()
    {
        const std::size_t hosts = req_.headers.count("host");
        if (hosts > 1)
            return bad(400, "multiple Host headers");
        if (req_.http_minor >= 1 && hosts != 1 && !absolute_form_)
            return bad(400, "HTTP/1.1 requires a Host header");

        bool te_present = false, te_chunked = false;
        unsigned long long content_length = 0;
        std::size_t cl_fields = 0;

        for (const auto& h : req_.headers) {
            if (h.first == "transfer-encoding") {
                te_present = true;
                // The final coding must be chunked, and chunked must appear once
                std::size_t codings = 0, chunked_at = 0, last = 0;
                std::size_t i = 0;
                while (i <= h.second.size()) {
                    std::size_t j = h.second.find(',', i);
                    if (j == std::string::npos)
                        j = h.second.size();
                    std::size_t b = i, e = j;
                    while (b < e && (h.second[b] == ' ' || h.second[b] == '\t')) ++b;
                    while (e > b && (h.second[e - 1] == ' ' || h.second[e - 1] == '\t')) --e;
                    if (b != e) {
                        ++codings;
                        last = codings;
                        if (detail::iequals(h.second.data() + b, e - b, "chunked", 7)) {
                            if (chunked_at)
                                return bad(400, "Transfer-Encoding applies chunked twice");
                            chunked_at = codings;
                        }
                    }
                    i = j + 1;
                }
                if (chunked_at && chunked_at == last)
                    te_chunked = true;
            } else if (h.first == "content-length") {
                ++cl_fields;
                unsigned long long n = 0;
                if (!parse_content_length(h.second, n))
                    return bad(400, "malformed Content-Length");
                if (cl_fields > 1 && n != content_length)
                    return bad(400, "conflicting Content-Length header fields");
                content_length = n;
            }
        }

        // RFC 9112 §6.1: both present is a smuggling attempt - 400 and close
        if (te_present && cl_fields)
            return bad(400, "Content-Length together with Transfer-Encoding");
        if (te_present && !te_chunked)
            return bad(501, "unsupported Transfer-Encoding");

        bool close_requested = false, keepalive_requested = false;
        for (const auto& h : req_.headers)
            if (h.first == "connection") {
                if (detail::has_token(h.second, "close"))
                    close_requested = true;
                if (detail::has_token(h.second, "keep-alive"))
                    keepalive_requested = true;
            }
        req_.keep_alive = (req_.http_minor >= 1) ? !close_requested : keepalive_requested;

        if (req_.http_minor >= 1)
            for (const auto& h : req_.headers)
                if (h.first == "expect") {
                    if (detail::has_token(h.second, "100-continue"))
                        expect_continue_ = true;
                    else
                        return bad(417, "unsupported Expect");
                }

        if (te_present) {
            framing_ = framing::chunked;
        } else if (content_length > 0) {
            if (content_length > lim_.max_body)
                return bad(413, "request body too large");
            framing_ = framing::identity;
            body_remaining_ = std::size_t(content_length);
            // Reserve modestly: a declared length is a claim, not a promise
            req_.body.reserve(std::min<std::size_t>(body_remaining_, 64u * 1024));
        } else {
            framing_ = framing::none;
        }
        return true;
    }

    bool parse_chunk_size(const char* b, const char* e, std::size_t& out)
    {
        const char* q = b;
        std::size_t n = 0;
        int digits = 0;
        while (q < e && *q != ';') {
            const int v = detail::hex_value(*q);
            if (v < 0)
                return bad(400, "malformed chunk size");
            if (n > (~std::size_t(0) - std::size_t(v)) / 16)
                return bad(413, "chunk size overflow");
            n = n * 16 + std::size_t(v);
            ++digits;
            ++q;
        }
        if (digits == 0)
            return bad(400, "missing chunk size");
        out = n;                                // chunk extensions after ';' are ignored
        return true;
    }

    parse_limits lim_{};
    request req_{};
    state st_ = state::start_line;
    framing framing_ = framing::none;
    std::size_t header_bytes_ = 0;
    std::size_t n_headers_ = 0;
    std::size_t body_remaining_ = 0;
    std::size_t chunk_remaining_ = 0;
    bool headers_done_ = false;
    bool expect_continue_ = false;
    bool started_ = false;
    bool absolute_form_ = false;
    int err_status_ = 0;
    const char* err_reason_ = nullptr;
};

} // namespace http
} // namespace snicholls

#endif /* http_parser_hpp */
