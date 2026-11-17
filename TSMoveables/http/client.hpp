//
//  client.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - an HTTP/1.1 client on the event loop: outbound
//  webhooks, calls to other services, anything that is a request and a
//  response:
//
//      snicholls::event_loop loop;
//      snicholls::http::client http(loop);
//      http.get("http://127.0.0.1:8080/health", [](snicholls::http::client_result r) {
//          if (r.is_ok()) std::cout << r->status << " " << r->body << "\n";
//          else           std::cout << r.unwrap_err_unchecked().what() << "\n";
//      });
//      loop.run();
//
//      auto r = snicholls::http::fetch({.url = "http://127.0.0.1:8080/x"});   // blocking, its own loop
//
//  What it does:
//
//    - Keep-alive with a pool per scheme://host:port: a finished connection
//      goes back idle and the next request to the same place reuses it. At
//      most max_connections_per_host are open per destination; further
//      requests queue behind them, in order.
//    - A reused connection can turn out dead - the server closed it while
//      idle, and that only shows when the request goes out. An idempotent
//      request (GET, HEAD, PUT, DELETE, OPTIONS) that got no byte of
//      response is then retried once on a fresh connection; a POST is not,
//      because it may already have taken effect.
//    - Responses are parsed by the server's own strict parser
//      (response_parser, the same code): Content-Length, chunked (trailers
//      kept), and bodies that run to the close; no body for HEAD, 1xx, 204
//      and 304. Interim 1xx responses are skipped.
//    - A timeout per request (connect included), an idle timeout for pooled
//      connections, and a body limit.
//    - https:// needs a transport_factory (TLS), the same one websocket_client
//      takes; without one it is an error, never a silent downgrade.
//
//  Threads: request() and its helpers may be called from any thread - the
//  work is posted to the loop - and the callback runs on the loop's thread.
//  Destroy the client on the loop's thread or after the loop has stopped;
//  requests still outstanding then get client_errc::cancelled.
//
//  Not here yet: HTTP/2, request bodies streamed in chunks, redirects
//  (follow them in the callback), cookies, compression. POSIX only, following
//  the event loop; SNICHOLLS_HAS_HTTP_CLIENT says which. C++20 (it returns
//  core's result).
//

#ifndef ts_moveables_http_client_hpp
#define ts_moveables_http_client_hpp

#include "../net/outbound.hpp"
#include "../net/url.hpp"

#if !SNICHOLLS_HAS_EVENT_LOOP
#define SNICHOLLS_HAS_HTTP_CLIENT 0
#else
#define SNICHOLLS_HAS_HTTP_CLIENT 1

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <snicholls/core/result.hpp>

#include "message.hpp"
#include "parser.hpp"

namespace snicholls {
namespace http {

enum class client_errc : std::uint8_t {
    bad_url = 1,         // not http(s)://host[:port]/..., or credentials in the URL
    bad_request,         // a header name or value that would break the request (CR, LF, ...)
    tls_unavailable,     // https:// with no transport_factory
    resolve_failed,
    connect_failed,
    tls_failed,
    timed_out,
    connection_closed,   // the connection ended before the response did
    protocol_error,      // the response did not parse
    body_too_large,
    cancelled,           // the client was destroyed or close()d first
};

inline const char* to_string(client_errc e) noexcept
{
    switch (e) {
    case client_errc::bad_url: return "bad URL";
    case client_errc::bad_request: return "bad request";
    case client_errc::tls_unavailable: return "https needs a transport_factory";
    case client_errc::resolve_failed: return "name did not resolve";
    case client_errc::connect_failed: return "connect failed";
    case client_errc::tls_failed: return "TLS failed";
    case client_errc::timed_out: return "timed out";
    case client_errc::connection_closed: return "connection closed before the response ended";
    case client_errc::protocol_error: return "malformed response";
    case client_errc::body_too_large: return "response body too large";
    case client_errc::cancelled: return "cancelled";
    }
    return "?";
}

class client_error : public std::runtime_error {
public:
    client_error(client_errc code, const std::string& detail)
        : std::runtime_error(std::string(to_string(code)) + (detail.empty() ? "" : ": " + detail)), code_(code)
    {
    }
    client_errc code() const noexcept { return code_; }

private:
    client_errc code_;
};

using client_result = result<response, client_error>;
using client_callback = std::function<void(client_result)>;

struct client_request {
    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::chrono::milliseconds timeout{0};        // 0: the client's request_timeout
};

struct client_config {
    net::transport_factory transport_factory;     // for https://; see websocket_client for an example
    std::chrono::milliseconds connect_timeout{10'000};
    std::chrono::milliseconds request_timeout{30'000};   // from submission to the last byte, connect included
    std::chrono::milliseconds idle_timeout{30'000};      // how long a pooled connection may wait for reuse
    std::size_t max_connections_per_host = 4;
    std::size_t max_idle_per_host = 4;
    parse_limits limits = [] {
        parse_limits l;
        l.max_body = 64u * 1024 * 1024;
        return l;
    }();
    std::vector<std::pair<std::string, std::string>> headers{{"User-Agent", "ts-moveables"}};   // on every request
};

struct client_stats {
    std::uint64_t requests = 0, responses = 0, failures = 0;
    std::uint64_t connections_opened = 0, reused = 0, retries = 0;
};

class client {
public:
    explicit client(event_loop& loop, client_config cfg = {})
        : c_(std::make_shared<core>(loop, std::move(cfg)))
    {
    }

    client(const client&) = delete;
    client& operator=(const client&) = delete;
    client(client&&) noexcept = default;
    client& operator=(client&&) noexcept = default;

    ~client()
    {
        if (c_)
            c_->shutdown();
    }

    // Any thread. `done` runs once, on the loop's thread
    void request(client_request req, client_callback done)
    {
        auto c = c_;
        c->poster.post([c, req = std::move(req), done = std::move(done)]() mutable {
            c->submit(std::move(req), std::move(done));
        });
    }

    void get(std::string url, client_callback done) { request({"GET", std::move(url), {}, {}, {}}, std::move(done)); }

    void post(std::string url, std::string body, std::string content_type, client_callback done)
    {
        request({"POST", std::move(url), {{"Content-Type", std::move(content_type)}}, std::move(body), {}},
                std::move(done));
    }

    // Fail everything outstanding with `cancelled` and close every connection. Loop thread (or loop stopped)
    void close() { c_->shutdown(); }

    // Loop thread (or loop stopped)
    client_stats stats() const { return c_->stats; }
    std::size_t idle_connections() const
    {
        std::size_t n = 0;
        for (const auto& [key, p] : c_->pools)
            n += p.idle.size();
        return n;
    }

private:
    struct pending {
        client_request req;
        net::url url;
        std::string wire;
        client_callback done;
        bool idempotent = false;
        int attempts = 0;
        std::shared_ptr<bool> live = std::make_shared<bool>(true);   // false once answered
        event_loop::timer deadline;    // owned here, so answering or dropping the request cancels it
    };

    struct connection {
        std::shared_ptr<net::outbound_connection> link;
        response_parser parser;
        std::shared_ptr<pending> current;
        bool reused = false;           // has carried a request before this one
        bool got_bytes = false;        // any byte of the current response
        event_loop::timer idle;
    };

    struct pool {
        std::vector<std::shared_ptr<connection>> idle;
        std::size_t active = 0;        // connections open or opening, idle ones included
        std::deque<std::shared_ptr<pending>> queue;
    };

    struct core : std::enable_shared_from_this<core> {
        core(event_loop& l, client_config c) : loop(&l), poster(l.make_poster()), cfg(std::move(c)) {}

        event_loop* loop;
        event_loop::poster poster;
        client_config cfg;
        std::map<std::string, pool> pools;
        client_stats stats;
        bool stopped = false;

        static bool clean(const std::string& s) noexcept
        {
            for (const char ch : s)
                if (ch == '\r' || ch == '\n' || ch == '\0')
                    return false;
            return true;
        }

        void answer(const std::shared_ptr<pending>& p, client_result r)
        {
            if (!*p->live)
                return;
            *p->live = false;
            p->deadline.cancel();
            by_request.erase(p.get());
            if (r.is_ok())
                ++stats.responses;
            else
                ++stats.failures;
            if (p->done)
                p->done(std::move(r));
        }

        void submit(client_request req, client_callback done)
        {
            ++stats.requests;
            auto p = std::make_shared<pending>();
            p->req = std::move(req);
            p->done = std::move(done);
            if (stopped)
                return answer(p, err(client_error(client_errc::cancelled, "")));
            if (!net::parse_url(p->req.url, p->url) || (p->url.scheme != "http" && p->url.scheme != "https"))
                return answer(p, err(client_error(client_errc::bad_url, p->req.url)));
            if (p->url.secure && !cfg.transport_factory)
                return answer(p, err(client_error(client_errc::tls_unavailable, p->req.url)));
            if (p->req.method.empty() || !clean(p->req.method) || p->req.method.find(' ') != std::string::npos)
                return answer(p, err(client_error(client_errc::bad_request, "method")));
            for (const auto& h : p->req.headers)
                if (h.first.empty() || !clean(h.first) || !clean(h.second) || h.first.find(':') != std::string::npos)
                    return answer(p, err(client_error(client_errc::bad_request, "header " + h.first)));
            const std::string& m = p->req.method;
            p->idempotent = m == "GET" || m == "HEAD" || m == "PUT" || m == "DELETE" || m == "OPTIONS";
            p->wire = serialise(*p);

            // The request's own deadline, from now, connect included
            const auto timeout = p->req.timeout.count() > 0 ? p->req.timeout : cfg.request_timeout;
            if (timeout.count() > 0) {
                std::weak_ptr<core> weak = shared_from_this();
                std::weak_ptr<pending> wp = p;
                p->deadline = loop->after(timeout);
                p->deadline.on_fire([weak, wp] {     // weak both ways: no cycle through the timer
                    auto self = weak.lock();
                    auto pp = wp.lock();
                    if (self && pp && *pp->live)
                        self->expire(pp);
                });
            }
            enqueue(p);
        }

        std::string serialise(const pending& p) const
        {
            std::string w;
            w.reserve(256 + p.req.body.size());
            w += p.req.method;
            w += ' ';
            w += p.url.target;
            w += " HTTP/1.1\r\nHost: ";
            w += p.url.authority();
            w += "\r\n";
            auto has = [&](const char* name) {
                for (const auto& h : p.req.headers)
                    if (detail::iequals(h.first.data(), h.first.size(), name, std::strlen(name)))
                        return true;
                return false;
            };
            for (const auto& h : cfg.headers)
                if (!has(h.first.c_str()))
                    w += h.first + ": " + h.second + "\r\n";
            for (const auto& h : p.req.headers)
                w += h.first + ": " + h.second + "\r\n";
            const bool body_method = p.req.method == "POST" || p.req.method == "PUT" || p.req.method == "PATCH";
            if ((!p.req.body.empty() || body_method) && !has("Content-Length"))
                w += "Content-Length: " + std::to_string(p.req.body.size()) + "\r\n";
            w += "\r\n";
            w += p.req.body;
            return w;
        }

        static std::string key_of(const net::url& u) { return u.scheme + "://" + u.host + ":" + u.port; }

        void enqueue(const std::shared_ptr<pending>& p)
        {
            pool& pl = pools[key_of(p->url)];
            while (!pl.idle.empty()) {
                auto conn = pl.idle.back();
                pl.idle.pop_back();
                if (conn->link && conn->link->open()) {
                    ++stats.reused;
                    return dispatch(conn, p);
                }
                --pl.active;                 // died while idle
            }
            if (pl.active < cfg.max_connections_per_host)
                return open_for(p);
            pl.queue.push_back(p);
        }

        void open_for(const std::shared_ptr<pending>& p)
        {
            pool& pl = pools[key_of(p->url)];
            ++pl.active;
            ++stats.connections_opened;
            auto conn = std::make_shared<connection>();
            all_connections.push_back(conn);
            if (all_connections.size() > 64)       // prune the dead now and then
                all_connections.erase(std::remove_if(all_connections.begin(), all_connections.end(),
                                                     [](const std::weak_ptr<connection>& w) { return w.expired(); }),
                                      all_connections.end());
            conn->parser.limits(cfg.limits);
            conn->link = std::make_shared<net::outbound_connection>(
                *loop, p->url.host, p->url.port, p->url.secure ? cfg.transport_factory : net::transport_factory{},
                cfg.connect_timeout);
            std::weak_ptr<core> weak = shared_from_this();
            std::weak_ptr<connection> wc = conn;
            const std::string key = key_of(p->url);
            conn->current = p;
            conn->link->on_open = [weak, wc] {
                auto self = weak.lock();
                auto c = wc.lock();
                if (self && c && c->current)
                    self->send(c);
            };
            conn->link->on_data = [weak, wc, key](std::string& in) {
                auto self = weak.lock();
                auto c = wc.lock();
                if (self && c)
                    self->on_data(key, c, in);
            };
            conn->link->on_close = [weak, conn, key](net::outbound_status why) {
                // The handler owns the connection until it has run: nothing else may
                if (auto self = weak.lock())
                    self->on_closed(key, conn, why);
            };
            conn->link->start();
        }

        void dispatch(const std::shared_ptr<connection>& conn, const std::shared_ptr<pending>& p)
        {
            conn->idle.cancel();
            conn->reused = true;
            conn->current = p;
            send(conn);
        }

        void send(const std::shared_ptr<connection>& conn)
        {
            by_request[conn->current.get()] = conn;
            conn->got_bytes = false;
            conn->parser.reset();
            conn->parser.expect_no_body(conn->current->req.method == "HEAD");
            ++conn->current->attempts;
            conn->link->write(conn->current->wire);
        }

        void on_data(const std::string& key, const std::shared_ptr<connection>& conn, std::string& in)
        {
            if (!conn->current) {
                // Bytes nobody asked for on an idle connection: it cannot be trusted with the next request
                in.clear();
                conn->link->close();
                return;
            }
            conn->got_bytes = true;
            for (;;) {
                std::size_t used = 0;
                const auto st = conn->parser.parse(in.data(), in.size(), used);
                in.erase(0, used);
                if (st == response_parser::status::need_more)
                    return;
                if (st == response_parser::status::failed) {
                    const auto p = conn->current;
                    conn->current.reset();
                    const auto code = conn->parser.error_status() == 413 ? client_errc::body_too_large
                                                                         : client_errc::protocol_error;
                    answer(p, err(client_error(code, conn->parser.error_reason())));
                    conn->link->close();
                    return;
                }
                response& r = conn->parser.message();
                if (r.status >= 100 && r.status < 200 && r.status != 101) {
                    // Interim (100 Continue, 103 Early Hints): the real response follows
                    const bool head = conn->current->req.method == "HEAD";
                    conn->parser.reset();
                    conn->parser.expect_no_body(head);
                    if (in.empty())
                        return;
                    continue;
                }
                const bool keep = r.keep_alive && r.status != 101 && in.empty();
                const auto p = conn->current;
                conn->current.reset();
                answer(p, ok(std::move(r)));
                if (!keep || stopped) {
                    conn->link->close();
                    return;
                }
                release(key, conn);
                return;
            }
        }

        // A finished connection: to the next queued request, else idle
        void release(const std::string& key, const std::shared_ptr<connection>& conn)
        {
            pool& pl = pools[key];
            if (!pl.queue.empty()) {
                auto next = pl.queue.front();
                pl.queue.pop_front();
                if (*next->live)
                    return dispatch(conn, next);
                return release(key, conn);
            }
            if (pl.idle.size() >= cfg.max_idle_per_host) {
                conn->link->close();
                return;
            }
            if (cfg.idle_timeout.count() > 0) {
                conn->idle = loop->after(cfg.idle_timeout);
                std::weak_ptr<connection> wc = conn;
                conn->idle.on_fire([wc] {
                    if (auto c = wc.lock())
                        if (!c->current)
                            c->link->close();
                });
            }
            pl.idle.push_back(conn);
        }

        void on_closed(const std::string& key, const std::shared_ptr<connection>& conn, net::outbound_status why)
        {
            pool& pl = pools[key];
            if (pl.active > 0)
                --pl.active;
            for (auto it = pl.idle.begin(); it != pl.idle.end(); ++it)
                if (*it == conn) {
                    pl.idle.erase(it);
                    break;
                }
            conn->idle.cancel();
            if (auto p = conn->current) {
                conn->current.reset();
                if (why == net::outbound_status::closed_by_peer && conn->parser.finish_eof() ==
                                                                       response_parser::status::have_message) {
                    answer(p, ok(std::move(conn->parser.message())));       // a body delimited by the close
                } else if (*p->live && conn->reused && !conn->got_bytes && p->idempotent && p->attempts < 2 &&
                           !stopped && (why == net::outbound_status::closed_by_peer ||
                                        why == net::outbound_status::transport_error)) {
                    ++stats.retries;             // the pooled connection was already dead: once more, fresh
                    open_for(p);
                    return;
                } else {
                    answer(p, err(client_error(map(why), net::to_string(why))));
                }
            }
            // A slot opened: the next queued request gets a fresh connection
            while (!pl.queue.empty() && pl.active < cfg.max_connections_per_host && !stopped) {
                auto next = pl.queue.front();
                pl.queue.pop_front();
                if (*next->live)
                    open_for(next);
            }
        }

        static client_errc map(net::outbound_status s) noexcept
        {
            switch (s) {
            case net::outbound_status::resolve_failed: return client_errc::resolve_failed;
            case net::outbound_status::connect_failed: return client_errc::connect_failed;
            case net::outbound_status::handshake_failed: return client_errc::tls_failed;
            case net::outbound_status::timed_out: return client_errc::timed_out;
            case net::outbound_status::closed_by_us: return client_errc::cancelled;
            default: return client_errc::connection_closed;
            }
        }

        void expire(const std::shared_ptr<pending>& p)
        {
            // Still queued: just answer. Connecting or in flight: answer, then drop that connection,
            // whose half-read response could not be told from the next one's
            for (auto& [key, pl] : pools)
                for (auto it = pl.queue.begin(); it != pl.queue.end(); ++it)
                    if (*it == p) {
                        pl.queue.erase(it);
                        return answer(p, err(client_error(client_errc::timed_out, "")));
                    }
            std::shared_ptr<connection> carrier;
            if (auto it = by_request.find(p.get()); it != by_request.end())
                carrier = it->second.lock();
            answer(p, err(client_error(client_errc::timed_out, "")));
            if (carrier && carrier->current == p) {
                carrier->current.reset();
                carrier->link->close();
            }
        }

        // Which connection carries which request, for timeouts
        std::map<const pending*, std::weak_ptr<connection>> by_request;
        // Every connection opened, so shutdown can close those still connecting too (each holds itself
        // through its close callback until it closes)
        std::vector<std::weak_ptr<connection>> all_connections;

        void shutdown()
        {
            if (stopped)
                return;
            stopped = true;
            std::vector<std::shared_ptr<connection>> idle;
            std::vector<std::shared_ptr<pending>> queued;
            for (auto& [key, pl] : pools) {
                for (auto& c : pl.idle)
                    idle.push_back(c);
                pl.idle.clear();
                for (auto& p : pl.queue)
                    queued.push_back(p);
                pl.queue.clear();
            }
            for (auto& p : queued)
                answer(p, err(client_error(client_errc::cancelled, "")));
            std::vector<std::shared_ptr<connection>> open;
            for (auto& wc : all_connections)
                if (auto c = wc.lock())
                    open.push_back(c);
            all_connections.clear();
            for (auto& c : open)
                if (c->link)
                    c->link->close();          // on_closed answers a request it carried with cancelled
        }
    };

    std::shared_ptr<core> c_;
};

// One request on a loop of its own, blocking the calling thread until it is answered
inline client_result fetch(client_request req, client_config cfg = {})
{
    event_loop loop;
    std::optional<client_result> out;
    {
        client c(loop, std::move(cfg));
        c.request(std::move(req), [&](client_result r) {
            out.emplace(std::move(r));
            loop.stop();
        });
        loop.run();
    }
    if (!out)
        return err(client_error(client_errc::cancelled, ""));
    return std::move(*out);
}

} // namespace http
} // namespace snicholls

#endif // SNICHOLLS_HAS_EVENT_LOOP

#endif // ts_moveables_http_client_hpp
