//
//  url.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - scheme://host[:port]/path?query, enough URL
//  parsing for a client to know where to connect and what to ask for, and no
//  more. Anything it does not understand is refused rather than guessed at,
//  because a guess here means connecting somewhere the caller did not ask for:
//
//      snicholls::net::url u;
//      if (snicholls::net::parse_url("https://api.example.com/v1/items?limit=5", u))
//          // u.scheme "https", u.host "api.example.com", u.port "443", u.target "/v1/items?limit=5"
//
//    - http and ws default to port 80, https and wss to 443, and those two
//      are `secure`; any other scheme is refused
//    - a bracketed IPv6 literal keeps its colons: http://[::1]:8080/
//    - user information (user:password@) is refused: a credential in a URL
//      ends up in logs, and headers are the place for it
//    - a fragment (#...) is dropped, as no client sends one
//
//  Shared by http::client and http::websocket_client. C++17, standard
//  library only.
//

#ifndef ts_moveables_net_url_hpp
#define ts_moveables_net_url_hpp

#include <string>

namespace snicholls {
namespace net {

struct url {
    std::string scheme;                 // lower case
    std::string host;                   // without brackets
    std::string port;                   // explicit or the scheme's default
    std::string target = "/";           // path and query, as sent on the request line
    bool secure = false;                // https, wss

    // The Host header's value: the port only when it is not the default
    std::string authority() const
    {
        const bool v6 = host.find(':') != std::string::npos;
        std::string a = v6 ? "[" + host + "]" : host;
        const bool default_port = (port == "80" && !secure) || (port == "443" && secure);
        if (!default_port)
            a += ":" + port;
        return a;
    }
};

inline bool parse_url(const std::string& text, url& out)
{
    const std::size_t sep = text.find("://");
    if (sep == std::string::npos || sep == 0)
        return false;
    std::string scheme = text.substr(0, sep);
    for (char& c : scheme)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    if (scheme == "http" || scheme == "ws") {
        out.port = "80";
        out.secure = false;
    } else if (scheme == "https" || scheme == "wss") {
        out.port = "443";
        out.secure = true;
    } else {
        return false;
    }
    out.scheme = scheme;

    const std::size_t start = sep + 3;
    std::size_t end = text.find_first_of("/?#", start);
    std::string authority = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (authority.empty() || authority.find('@') != std::string::npos)
        return false;
    std::string rest = end == std::string::npos ? std::string() : text.substr(end);
    if (const std::size_t hash = rest.find('#'); hash != std::string::npos)
        rest.erase(hash);
    if (rest.empty() || rest[0] == '?')
        rest.insert(rest.begin(), '/');
    for (const char c : rest) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u == 0x7f)
            return false;                   // a space or control character would split the request line
    }
    out.target = rest;

    // A bracketed IPv6 literal keeps its colons; only a colon after it separates the port
    if (authority[0] == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string::npos)
            return false;
        out.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':')
                return false;
            out.port = authority.substr(close + 2);
        }
    } else {
        const std::size_t colon = authority.rfind(':');
        if (colon == std::string::npos) {
            out.host = authority;
        } else {
            out.host = authority.substr(0, colon);
            out.port = authority.substr(colon + 1);
        }
    }
    if (out.host.empty() || out.port.empty())
        return false;
    for (const char c : out.port)
        if (c < '0' || c > '9')
            return false;
    return true;
}

} // namespace net
} // namespace snicholls

#endif // ts_moveables_net_url_hpp
