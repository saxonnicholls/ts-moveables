//
//  http/message.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  What an HTTP message is: status codes, methods, header lists, request and
//  response - and the byte-level plumbing they are built from.
//
//  Separated because none of it touches IO, a socket, or a loop. That is what
//  makes the parser testable with a string and the router benchmarkable
//  without a kernel, and it is why this file can be read on its own.
//

#ifndef http_message_hpp
#define http_message_hpp



#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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

} // namespace http
} // namespace snicholls

#endif /* http_message_hpp */
