//
//  http/config.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  The knobs, and what the observation taps carry.
//

#ifndef http_config_hpp
#define http_config_hpp

#include "parser.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace snicholls {
namespace http {

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

// --------------------------------------------------------------- origin policy
//
// Who is allowed to open a cross-origin connection to this server.
//
// This exists because of one asymmetry that surprises nearly everyone: browsers
// do NOT apply the same-origin policy to WebSockets. A fetch() to
// http://127.0.0.1:7333/ is stopped before it is sent; a WebSocket to
// ws://127.0.0.1:7333/ is not. So any page the developer happens to visit can
// open a socket to a loopback-bound server and read whatever it streams - and
// if that server replays history on connect, the backlog as well. Binding to
// loopback feels like a boundary and is not one: the browser is already on the
// loopback side, and it is the browser doing the asking.
//
// The handshake is therefore the only place that can say no, and the only
// evidence it has is the Origin header - which is worth exactly as much as the
// fact that a browser will not let a page lie about it or leave it off.
//
// The default reads that evidence the useful way (see origin_policy below):
//
//   no Origin header  -> allow. curl, a native client, a webhook sender, another
//                        service: no browser is involved, so there is no
//                        drive-by to stop, and none of them send one. This is
//                        not a hole a page can climb through - a browser always
//                        attaches Origin to a cross-origin socket or POST, and
//                        script cannot remove it.
//   loopback Origin   -> allow. A page served from localhost is the tool's own
//                        console or viewer, which is the common shape for a
//                        local server and the case a blanket reject breaks.
//   listed in `allow` -> allow. The deployed browser app's real origin.
//   anything else     -> reject, before the upgrade or the write.
//
// `null` is not loopback and not special: sandboxed iframes, file:// pages and
// some redirect chains send it, it names nobody, and so it faces the allowlist
// like any other value.
//
// The same policy answers for the write side. A cross-origin POST - including
// the `no-cors`, text/plain "simple request" that a page can send without a
// preflight and whose response it cannot read - still carries Origin, so
// checking it here closes the CSRF path into an ingest endpoint without the
// endpoint having to speak CORS at all.

namespace detail {

// Split a serialised origin into scheme, host and port.
//
// An origin is scheme + host + optional port and nothing else (RFC 6454 §6.1).
// The strictness is the point: anything carrying a path, userinfo, query or
// fragment is not an origin, and the only reason to send one that does is to
// get a lenient reader to find "localhost" somewhere inside
// "http://evil.com/@localhost" and stop looking. So a value with those bytes in
// it is refused outright rather than parsed generously.
inline bool split_origin(std::string_view o,
                         std::string_view& scheme,
                         std::string_view& host,
                         std::string_view& port) noexcept
{
    const std::size_t sep = o.find("://");
    if (sep == std::string_view::npos)
        return false;
    scheme = o.substr(0, sep);
    if (scheme.empty())
        return false;

    std::string_view auth = o.substr(sep + 3);
    if (auth.empty() || auth.find_first_of("/\\?#@") != std::string_view::npos)
        return false;

    if (auth.front() == '[') {                      // [::1] or [::1]:8080
        const std::size_t close = auth.find(']');
        if (close == std::string_view::npos)
            return false;
        host = auth.substr(1, close - 1);
        const std::string_view rest = auth.substr(close + 1);
        if (rest.empty())
            port = std::string_view{};
        else if (rest.front() == ':')
            port = rest.substr(1);
        else
            return false;
    } else {
        const std::size_t colon = auth.find(':');
        if (colon == std::string_view::npos) {
            host = auth;
            port = std::string_view{};
        } else {
            host = auth.substr(0, colon);
            port = auth.substr(colon + 1);
            if (port.find(':') != std::string_view::npos)
                return false;                       // unbracketed IPv6
        }
    }
    if (host.empty())
        return false;
    for (const char c : port)
        if (!is_digit(c))
            return false;
    return true;
}

// Is this host the local machine? "localhost" by name, the whole 127.0.0.0/8
// literal range (127.0.0.1 is the usual one, but 127.0.0.2 and friends are
// equally local and equally reachable), and IPv6 ::1.
//
// Matched against the parsed host and never by substring or suffix, which is
// the whole trick these checks get caught by: "localhost.evil.com" is a name
// its owner controls, "127.0.0.1.evil.com" likewise, and neither is local.
inline bool is_loopback_host(std::string_view h) noexcept
{
    if (iequals(h.data(), h.size(), "localhost", 9))
        return true;
    if (h == "::1" || h == "0:0:0:0:0:0:0:1")
        return true;

    // Dotted quad, fully consumed, first octet 127. Anything that is not four
    // plain decimal octets is a name rather than an address, and a name only
    // reaches loopback by resolving there - which is not ours to assume.
    unsigned first = 0;
    std::size_t i = 0;
    for (int k = 0; k < 4; ++k) {
        if (i >= h.size() || !is_digit(h[i]))
            return false;
        unsigned v = 0, digits = 0;
        while (i < h.size() && is_digit(h[i])) {
            v = v * 10 + unsigned(h[i] - '0');
            if (++digits > 3 || v > 255)
                return false;
            ++i;
        }
        if (k == 0)
            first = v;
        if (k < 3) {
            if (i >= h.size() || h[i] != '.')
                return false;
            ++i;
        }
    }
    return i == h.size() && first == 127;
}

} // namespace detail

struct origin_policy {
    // Off makes every check below pass. For a server that genuinely wants any
    // origin, and would rather say so once here than be talked out of the
    // default one exception at a time.
    bool enforce        = true;

    bool allow_missing  = true;         // no Origin header at all
    bool allow_loopback = true;         // localhost, 127.0.0.0/8, ::1

    // Exact serialised origins - "https://app.example.com", scheme and host and
    // port, no path and no wildcards. Compared case-insensitively, because a
    // browser lowercases what it sends and a hand-written list should not have
    // to remember that.
    std::vector<std::string> allow;

    // An extra allow, for a rule the list cannot spell: a wildcard subdomain, a
    // value read from configuration at run time. It can only widen the policy -
    // it is consulted after the built-in allows have all declined, and cannot
    // veto one that has already said yes.
    std::function<bool(std::string_view)> allow_if;

    static origin_policy any()
    {
        origin_policy p;
        p.enforce = false;
        return p;
    }

    // `origin` is the header value, or nullptr when the request carried no
    // Origin at all. The distinction matters: an empty Origin: header is a
    // header naming no origin, not the absence of one, so it is not the
    // non-browser case and falls through to the allowlist.
    bool allows(const std::string* origin) const
    {
        if (!enforce)
            return true;
        if (!origin)
            return allow_missing;

        const std::string_view o(*origin);
        if (allow_loopback) {
            std::string_view scheme, host, port;
            if (detail::split_origin(o, scheme, host, port) &&
                detail::is_loopback_host(host))
                return true;
        }
        for (const auto& a : allow)
            if (detail::iequals(a.data(), a.size(), o.data(), o.size()))
                return true;
        return allow_if && allow_if(o);
    }

    // The same question asked of a request directly, which is how a handler
    // wants to ask it:
    //
    //     if (!policy.allows(req)) { res.send(403, ...); return; }
    //
    // Worth having as one call because the alternative spelling reaches through
    // two objects to find the header, and a security check that is tedious to
    // write correctly is one that gets written incorrectly or skipped.
    bool allows(const request& req) const { return allows(req.header("origin")); }
};

} // namespace http
} // namespace snicholls

#endif /* http_config_hpp */
