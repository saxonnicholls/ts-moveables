//
//  http_server.hpp
//  TSMoveables
//
//  Copyright 2026 Saxon Herschel Nicholls
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

#include "event_loop.hpp"
#include "moveable_signal.hpp"

#if !SNICHOLLS_HAS_EVENT_LOOP
#define SNICHOLLS_HAS_HTTP_SERVER 0
#else
#define SNICHOLLS_HAS_HTTP_SERVER 1

#include <algorithm>
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

namespace snicholls {
namespace http {

// ---------------------------------------------------------------- status codes

// The complete status line for the codes a server actually emits in volume.
// Serialising a response otherwise costs five appends and an integer format
// for a string that was knowable at compile time; this makes the common case
// a single append of a literal. Returns null for anything not worth a slot,
// and the general path handles those.
constexpr const char* status_line(int code) noexcept
{
    switch (code) {
    case 200: return "HTTP/1.1 200 OK\r\n";
    case 201: return "HTTP/1.1 201 Created\r\n";
    case 204: return "HTTP/1.1 204 No Content\r\n";
    case 301: return "HTTP/1.1 301 Moved Permanently\r\n";
    case 302: return "HTTP/1.1 302 Found\r\n";
    case 304: return "HTTP/1.1 304 Not Modified\r\n";
    case 400: return "HTTP/1.1 400 Bad Request\r\n";
    case 401: return "HTTP/1.1 401 Unauthorized\r\n";
    case 403: return "HTTP/1.1 403 Forbidden\r\n";
    case 404: return "HTTP/1.1 404 Not Found\r\n";
    case 405: return "HTTP/1.1 405 Method Not Allowed\r\n";
    case 500: return "HTTP/1.1 500 Internal Server Error\r\n";
    case 503: return "HTTP/1.1 503 Service Unavailable\r\n";
    default:  return nullptr;
    }
}

constexpr const char* status_text(int code) noexcept
{
    switch (code) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 422: return "Unprocessable Content";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}

// --------------------------------------------------------------------- methods

enum class method { get, head, post, put, del, patch, options, connect, trace, unknown };

inline const char* to_string(method m) noexcept
{
    switch (m) {
    case method::get:     return "GET";
    case method::head:    return "HEAD";
    case method::post:    return "POST";
    case method::put:     return "PUT";
    case method::del:     return "DELETE";
    case method::patch:   return "PATCH";
    case method::options: return "OPTIONS";
    case method::connect: return "CONNECT";
    case method::trace:   return "TRACE";
    default:              return "UNKNOWN";
    }
}

inline method parse_method(const std::string& s) noexcept
{
    if (s == "GET")     return method::get;
    if (s == "HEAD")    return method::head;
    if (s == "POST")    return method::post;
    if (s == "PUT")     return method::put;
    if (s == "DELETE")  return method::del;
    if (s == "PATCH")   return method::patch;
    if (s == "OPTIONS") return method::options;
    if (s == "CONNECT") return method::connect;
    if (s == "TRACE")   return method::trace;
    return method::unknown;
}

// ------------------------------------------------------------- string plumbing

namespace detail {

inline char lower_ascii(char c) noexcept { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

inline void ascii_lower(std::string& s) noexcept
{
    for (auto& c : s)
        c = lower_ascii(c);
}

inline bool iequals(const char* a, std::size_t na, const char* b, std::size_t nb) noexcept
{
    if (na != nb)
        return false;
    for (std::size_t i = 0; i < na; ++i)
        if (lower_ascii(a[i]) != lower_ascii(b[i]))
            return false;
    return true;
}

inline bool iequals(const std::string& a, const char* b) noexcept
{
    return iequals(a.data(), a.size(), b, std::strlen(b));
}

constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

// Character classification, resolved at compile time.
//
// Every byte of every method and every header field name is classified, and
// every byte of a chunk size and percent escape is hex-decoded - so these are
// the hottest predicates in the parser. Built as 256-entry tables by a
// constexpr function, the run-time cost becomes one indexed load instead of a
// chain of comparisons, and the tables land in .rodata rather than being
// assembled at startup.
//
// constexpr, not consteval: this library is C++17, where consteval does not
// exist. It would express "compile time or fail" more exactly, but generates
// identical code, so it is not worth a version fence.

struct char_tables {
    bool token[256]{};
    signed char hex[256]{};
};

constexpr char_tables make_char_tables() noexcept
{
    char_tables t{};
    for (int i = 0; i < 256; ++i) {
        t.token[i] = false;
        t.hex[i] = -1;
    }
    for (int c = 'a'; c <= 'z'; ++c) t.token[c] = true;
    for (int c = 'A'; c <= 'Z'; ++c) t.token[c] = true;
    for (int c = '0'; c <= '9'; ++c) t.token[c] = true;
    // RFC 9110 tchar. Anything outside this in a field name is a hard error,
    // which is what keeps "Content-Length : 5" - and its desync - out
    const char extra[] = {'!', '#', '$', '%', '&', '\'', '*',
                          '+', '-', '.', '^', '_', '`', '|', '~'};
    for (char c : extra)
        t.token[static_cast<unsigned char>(c)] = true;

    for (int c = '0'; c <= '9'; ++c) t.hex[c] = static_cast<signed char>(c - '0');
    for (int c = 'a'; c <= 'f'; ++c) t.hex[c] = static_cast<signed char>(c - 'a' + 10);
    for (int c = 'A'; c <= 'F'; ++c) t.hex[c] = static_cast<signed char>(c - 'A' + 10);
    return t;
}

inline constexpr char_tables kChars = make_char_tables();

inline bool is_token_char(char c) noexcept
{
    return kChars.token[static_cast<unsigned char>(c)];
}

inline int hex_value(char c) noexcept
{
    return kChars.hex[static_cast<unsigned char>(c)];
}

// memmem is a GNU extension; this is the two-line portable equivalent
inline const void* find_bytes(const void* hay, std::size_t n,
                              const char* needle, std::size_t m) noexcept
{
    if (m == 0 || n < m)
        return nullptr;
    const char* p = static_cast<const char*>(hay);
    const char* const end = p + (n - m) + 1;
    while ((p = static_cast<const char*>(std::memchr(p, needle[0], std::size_t(end - p)))) != nullptr) {
        if (std::memcmp(p, needle, m) == 0)
            return p;
        ++p;
        if (p >= end)
            break;
    }
    return nullptr;
}

// Strict: a malformed escape is a 400, not a literal '%'
inline bool percent_decode(const char* p, std::size_t n, std::string& out, bool plus_is_space)
{
    out.clear();
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const char c = p[i];
        if (c == '%') {
            if (i + 2 >= n)
                return false;
            const int hi = hex_value(p[i + 1]), lo = hex_value(p[i + 2]);
            if (hi < 0 || lo < 0)
                return false;
            out.push_back(char((hi << 4) | lo));
            i += 2;
        } else if (c == '+' && plus_is_space) {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    return true;
}

inline void append_uint(std::string& out, unsigned long long v)
{
    char buf[24];
    int n = 0;
    if (v == 0)
        buf[n++] = '0';
    while (v) {
        buf[n++] = char('0' + (v % 10));
        v /= 10;
    }
    while (n)
        out.push_back(buf[--n]);
}

// Response splitting defence: a CR or LF smuggled into a header value would
// forge a whole extra response. They never survive serialisation.
inline void append_sanitised(std::string& out, const std::string& s)
{
    for (char c : s)
        out.push_back((c == '\r' || c == '\n') ? ' ' : c);
}

// Does a comma-separated field value contain this token?
inline bool has_token(const std::string& value, const char* token)
{
    const std::size_t tn = std::strlen(token);
    std::size_t i = 0;
    while (i <= value.size()) {
        std::size_t j = value.find(',', i);
        if (j == std::string::npos)
            j = value.size();
        std::size_t b = i, e = j;
        while (b < e && (value[b] == ' ' || value[b] == '\t')) ++b;
        while (e > b && (value[e - 1] == ' ' || value[e - 1] == '\t')) --e;
        if (iequals(value.data() + b, e - b, token, tn))
            return true;
        i = j + 1;
    }
    return false;
}

} // namespace detail

// ---------------------------------------------------------------- header lists

class header_list {
public:
    using entry = std::pair<std::string, std::string>;

    void add(std::string name, std::string value)
    {
        if (v_.capacity() == 0)
            v_.reserve(16);                     // typical request; avoids the growth walk
        v_.emplace_back(std::move(name), std::move(value));
    }

    void set(std::string name, std::string value)
    {
        for (auto& e : v_)
            if (detail::iequals(e.first.data(), e.first.size(), name.data(), name.size())) {
                e.second = std::move(value);
                return;
            }
        add(std::move(name), std::move(value));
    }

    const std::string* find(const char* name) const noexcept
    {
        const std::size_t n = std::strlen(name);
        for (const auto& e : v_)
            if (detail::iequals(e.first.data(), e.first.size(), name, n))
                return &e.second;
        return nullptr;
    }

    bool has(const char* name) const noexcept { return find(name) != nullptr; }

    std::size_t count(const char* name) const noexcept
    {
        const std::size_t n = std::strlen(name);
        std::size_t k = 0;
        for (const auto& e : v_)
            if (detail::iequals(e.first.data(), e.first.size(), name, n))
                ++k;
        return k;
    }

    std::size_t size() const noexcept { return v_.size(); }
    bool empty() const noexcept { return v_.empty(); }
    // clear(), not destroy: the vector keeps its capacity so the next request
    // on this connection reuses it instead of allocating again
    void clear() noexcept { v_.clear(); }

    std::vector<entry>::const_iterator begin() const noexcept { return v_.begin(); }
    std::vector<entry>::const_iterator end() const noexcept { return v_.end(); }

private:
    std::vector<entry> v_;
};

// -------------------------------------------------------------------- messages

class request {
public:
    http::method method = http::method::unknown;
    std::string method_text;
    std::string target;                 // request-target exactly as sent
    std::string path;                   // percent-decoded, no query
    std::string query;                  // raw query string, no '?'
    int http_major = 1;
    int http_minor = 1;
    header_list headers;
    std::string body;
    bool keep_alive = true;
    std::unordered_map<std::string, std::string> params;    // ":name" captures

    // Reset for reuse without releasing a single allocation. The strings keep
    // their capacity and the header vector keeps its buffer, so a connection
    // serving many requests allocates for them once rather than every time.
    void clear() noexcept
    {
        method = http::method::unknown;
        method_text.clear();
        target.clear();
        path.clear();
        query.clear();
        http_major = http_minor = 1;
        headers.clear();
        body.clear();
        keep_alive = true;
        params.clear();
    }

    const std::string* header(const char* name) const noexcept { return headers.find(name); }

    std::string header_or(const char* name, const char* fallback = "") const
    {
        const std::string* v = headers.find(name);
        return v ? *v : std::string(fallback);
    }

    std::string param(const std::string& name) const
    {
        auto it = params.find(name);
        return it == params.end() ? std::string{} : it->second;
    }

    bool has_param(const std::string& name) const { return params.find(name) != params.end(); }

    // Percent-decoded query parameter; '+' means space here, unlike in a path
    std::string query_param(const std::string& name, const char* fallback = "") const
    {
        std::size_t i = 0;
        while (i < query.size()) {
            std::size_t amp = query.find('&', i);
            if (amp == std::string::npos)
                amp = query.size();
            std::size_t eq = query.find('=', i);
            const bool has_value = (eq != std::string::npos && eq < amp);
            const std::size_t key_end = has_value ? eq : amp;
            std::string key;
            if (detail::percent_decode(query.data() + i, key_end - i, key, true) && key == name) {
                std::string value;
                if (has_value)
                    detail::percent_decode(query.data() + eq + 1, amp - eq - 1, value, true);
                return value;
            }
            i = amp + 1;
        }
        return std::string(fallback);
    }
};

class response {
public:
    int status = 200;
    header_list headers;
    std::string body;

    response() = default;
    explicit response(int s) : status(s) {}

    response& set(std::string name, std::string value)
    {
        headers.set(std::move(name), std::move(value));
        return *this;
    }

    response& type(std::string content_type) { return set("Content-Type", std::move(content_type)); }

    response& content(std::string b, std::string content_type = "text/plain; charset=utf-8")
    {
        body = std::move(b);
        return type(std::move(content_type));
    }
};

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

// ------------------------------------------------------------------ the knobs

struct server_config {
    parse_limits limits{};
    std::size_t read_buffer      = 64u * 1024;      // one shared buffer, not one per connection
    std::size_t write_high_water = 1024u * 1024;    // pause reading above this backlog
    int backlog                  = 1024;
    int accept_burst             = 64;              // accepts per readiness, so one loop cannot starve others
    int reads_per_event          = 8;
    std::chrono::seconds idle_timeout{60};
    std::chrono::seconds request_timeout{20};       // slowloris
    std::chrono::milliseconds sweep_interval{1000};
    bool tcp_nodelay             = true;
    bool reuse_port              = false;           // one server per thread, kernel-balanced
    std::size_t max_connections  = 0;               // 0: unlimited
    std::string server_name      = "ts-moveables";
};

// ---------------------------------------------------------------- observation

struct connection_info {
    int fd = -1;
    std::string peer;
    std::uint16_t peer_port = 0;
    std::uint64_t id = 0;
};

struct access_entry {
    std::string method;
    std::string path;
    std::string query;
    int status = 0;
    std::size_t response_bytes = 0;
    std::size_t request_bytes = 0;
    double duration_ms = 0.0;
    int fd = -1;
    std::uint64_t stream = 0;
};

// ------------------------------------------------------------------ delegates
//
// Transport: how bytes reach us. Plaintext today; a TLS engine is the same
// shape (ciphertext in, plaintext out) because it never touches the socket -
// the reactor owns all IO. That is what makes every backend testable without
// a network, and what makes the backend a run-time choice.

class transport_delegate {
public:
    virtual ~transport_delegate() = default;
    virtual const char* name() const noexcept = 0;

    // Wire bytes arrived: append application bytes to app_in, and any bytes
    // the transport itself owes the peer (handshake, alerts) to wire_out
    virtual bool wire_in(const char* data, std::size_t n,
                         std::string& app_in, std::string& wire_out) = 0;

    // The application wants to send bytes: append wire bytes to wire_out
    virtual bool app_out(const char* data, std::size_t n, std::string& wire_out) = 0;

    virtual bool established() const noexcept { return true; }
    // The protocol ALPN settled on, or an **empty string** when nothing was
    // negotiated - a plaintext connection, or a client that offered no list.
    // Those two cases must stay distinguishable: choosing the protocol
    // delegate by ALPN means "the client asked for h2" and "the client said
    // nothing, so default to http/1.1" are different answers, and collapsing
    // them into the literal "http/1.1" throws away the only bit that matters.
    virtual const char* alpn() const noexcept { return ""; }
    virtual void shutdown(std::string& /*wire_out*/) {}
};

class plain_transport final : public transport_delegate {
public:
    const char* name() const noexcept override { return "plain"; }

    bool wire_in(const char* data, std::size_t n, std::string& app_in, std::string&) override
    {
        app_in.append(data, n);
        return true;
    }

    bool app_out(const char* data, std::size_t n, std::string& wire_out) override
    {
        wire_out.append(data, n);
        return true;
    }
};

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

    // Bytes queued for this connection but not yet handed to the kernel
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

    std::size_t pending_bytes() const noexcept { return out.size() - sent; }

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
            flush();
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
    auto s = s_.lock();
    return s ? s->pending_bytes() : 0;
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
