//
//  interfaces/transport_delegate_conformance.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  The one conformance kit for transports: the semantic half of the contract
//  that concepts/transport_delegate.hpp can only check syntactically. Every backend runs
//  through it - plain_transport, the TLS backends, and any implementation
//  elsewhere in the family - in either form, because it is a template over
//  anything transport_like, and a transport_delegate& is.
//
//      auto failures = snicholls::http::check_transport_pair(client, server, {"h2"});
//      assert(failures.empty());                   // or print them: each says what broke
//
//  A pair is a client and a server transport wired back to back in memory:
//  no socket, no event loop, the bytes each owes the other shuttled across
//  until neither has any. What it checks:
//
//    - name() is a non-empty string, before and after the handshake
//    - the client's start() succeeds, and the handshake completes within a
//      bounded number of exchanges: both sides established(), neither failed
//    - both sides agree on alpn(), and it is the one expected when one is
//      given; empty means nothing was negotiated, never a made-up default
//    - application bytes arrive intact and in order, both directions, at
//      1 byte, 1000, 64 KiB and 1 MiB, including bytes queued before the
//      peer has read the previous ones
//    - an app_out of zero bytes succeeds and changes nothing
//    - shutdown() can be called on both sides, and what it emits is accepted
//      or refused by the peer without the peer crashing
//
//  check_rejects_garbage(fresh_server) is the extra check for a transport that
//  protects anything: bytes that are not a handshake must fail wire_in or
//  leave it unestablished, and must never surface as plaintext.
//
//  Returns the failures as messages rather than asserting, so a test framework
//  of any kind can report them. C++17.
//

#ifndef interfaces_transport_delegate_conformance_hpp
#define interfaces_transport_delegate_conformance_hpp

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "../concepts/transport_delegate.hpp"

namespace snicholls {
namespace http {

struct transport_conformance_options {
    const char* expected_alpn = nullptr;   // nullptr: only that both sides agree
    int max_rounds = 64;                   // exchanges allowed for the handshake and for each payload
};

namespace detail {

template <typename A, typename B>
struct transport_wire {
    A& client;
    B& server;
    std::string to_server, to_client;      // wire bytes in flight
    std::string at_server, at_client;      // plaintext delivered
    bool failed = false;
    std::string why;

    // Deliver everything in flight, both ways, until nothing is
    bool pump()
    {
        for (int i = 0; i < 1024 && !failed && (!to_server.empty() || !to_client.empty()); ++i) {
            if (!to_server.empty()) {
                std::string in;
                in.swap(to_server);
                if (!server.wire_in(in.data(), in.size(), at_server, to_client)) {
                    failed = true;
                    why = "server wire_in failed";
                }
            }
            if (!failed && !to_client.empty()) {
                std::string in;
                in.swap(to_client);
                if (!client.wire_in(in.data(), in.size(), at_client, to_server)) {
                    failed = true;
                    why = "client wire_in failed";
                }
            }
        }
        return !failed;
    }
};

inline std::string conformance_payload(std::size_t n, unsigned seed)
{
    std::string s(n, '\0');
    unsigned x = seed * 2654435761u + 1u;
    for (auto& c : s) {
        x = x * 1103515245u + 12345u;
        c = char(x >> 16);
    }
    return s;
}

} // namespace detail

template <typename Client, typename Server>
std::vector<std::string> check_transport_pair(Client& client, Server& server,
                                              transport_conformance_options opt = {})
{
    static_assert(is_transport_like_v<Client> && is_transport_like_v<Server>,
                  "check_transport_pair: both sides must be transport_like");
    std::vector<std::string> fail;
    auto named = [&](const char* side, const char* n) {
        if (!n || !*n)
            fail.push_back(std::string(side) + ": name() is empty");
    };
    named("client", client.name());
    named("server", server.name());

    detail::transport_wire<Client, Server> w{client, server, {}, {}, {}, {}, false, {}};
    if (!client.start(w.to_server))
        fail.push_back("client: start() failed");
    std::string ignored;
    if (!server.start(ignored))
        fail.push_back("server: start() failed (a server's start is a no-op that succeeds)");
    if (!ignored.empty())
        w.to_client += ignored;

    for (int round = 0; round < opt.max_rounds && !(client.established() && server.established()); ++round)
        if (!w.pump())
            break;
    w.pump();
    if (w.failed)
        fail.push_back("handshake: " + w.why);
    if (!client.established() || !server.established()) {
        fail.push_back(std::string("handshake: not established after ") + std::to_string(opt.max_rounds) +
                       " exchanges (client " + (client.established() ? "yes" : "no") + ", server " +
                       (server.established() ? "yes" : "no") + ")");
        return fail;
    }
    if (!w.at_server.empty() || !w.at_client.empty())
        fail.push_back("handshake: plaintext appeared before any was sent");

    const char* ca = client.alpn();
    const char* sa = server.alpn();
    if (!ca || !sa)
        fail.push_back("alpn(): null (empty string means none)");
    else if (std::strcmp(ca, sa) != 0)
        fail.push_back(std::string("alpn(): client \"") + ca + "\" and server \"" + sa + "\" disagree");
    else if (opt.expected_alpn && std::strcmp(ca, opt.expected_alpn) != 0)
        fail.push_back(std::string("alpn(): \"") + ca + "\", expected \"" + opt.expected_alpn + "\"");
    named("client (established)", client.name());
    named("server (established)", server.name());

    // Application data both ways, several sizes, each direction sent in two
    // writes queued before the peer reads either
    unsigned seed = 1;
    for (const std::size_t n : {std::size_t(1), std::size_t(1000), std::size_t(64u << 10), std::size_t(1u << 20)}) {
        for (int dir = 0; dir < 2; ++dir) {
            const std::string a = detail::conformance_payload(n, seed++);
            const std::string b = detail::conformance_payload(n / 2 + 1, seed++);
            const bool c2s = dir == 0;
            std::string& wire = c2s ? w.to_server : w.to_client;
            const bool sent = c2s ? (client.app_out(a.data(), a.size(), wire) && client.app_out(b.data(), b.size(), wire))
                                  : (server.app_out(a.data(), a.size(), wire) && server.app_out(b.data(), b.size(), wire));
            const std::string what = std::string(c2s ? "client to server" : "server to client") + ", " +
                                     std::to_string(n) + " bytes";
            if (!sent) {
                fail.push_back(what + ": app_out failed");
                continue;
            }
            for (int round = 0; round < opt.max_rounds && (c2s ? w.at_server : w.at_client).size() < a.size() + b.size();
                 ++round)
                if (!w.pump())
                    break;
            std::string& got = c2s ? w.at_server : w.at_client;
            if (w.failed)
                fail.push_back(what + ": " + w.why);
            else if (got != a + b)
                fail.push_back(what + ": " + std::to_string(got.size()) + " bytes arrived, " +
                               std::to_string(a.size() + b.size()) + " sent" +
                               (got.size() == a.size() + b.size() ? ", different" : ""));
            got.clear();
        }
    }

    // Nothing to send is not an error, and sends nothing a peer would read as data
    std::string empty_wire;
    if (!client.app_out("", 0, empty_wire))
        fail.push_back("client: app_out of 0 bytes failed");
    w.to_server += empty_wire;
    w.pump();
    if (!w.at_server.empty())
        fail.push_back("client: app_out of 0 bytes delivered plaintext");

    // Shutdown on both sides: the peer may accept or refuse what it says, but must survive it
    std::string bye;
    client.shutdown(bye);
    std::string sink_app, sink_wire;
    (void)server.wire_in(bye.data(), bye.size(), sink_app, sink_wire);
    if (!sink_app.empty())
        fail.push_back("client: shutdown() delivered plaintext");
    bye.clear();
    server.shutdown(bye);
    sink_app.clear();
    sink_wire.clear();
    (void)client.wire_in(bye.data(), bye.size(), sink_app, sink_wire);
    if (!sink_app.empty())
        fail.push_back("server: shutdown() delivered plaintext");
    return fail;
}

// For a transport that protects anything (not plain_transport): bytes that
// are not a handshake fail it, and never come out as plaintext
template <typename Server>
std::vector<std::string> check_rejects_garbage(Server& fresh_server)
{
    static_assert(is_transport_like_v<Server>, "check_rejects_garbage: the server must be transport_like");
    std::vector<std::string> fail;
    const std::string junk = "GET / HTTP/1.1\r\nHost: x\r\n\r\n" + detail::conformance_payload(512, 99);
    std::string app, wire;
    const bool ok = fresh_server.wire_in(junk.data(), junk.size(), app, wire);
    if (!app.empty())
        fail.push_back("garbage: surfaced as plaintext");
    if (ok && fresh_server.established())
        fail.push_back("garbage: accepted and established");
    return fail;
}

} // namespace http
} // namespace snicholls

#endif /* interfaces_transport_delegate_conformance_hpp */
