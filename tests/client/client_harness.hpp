//
//  client_harness.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - what the HTTP client's test files share: a live http::server on a thread, a scripted
//  raw-socket server, and a driver that runs requests on a loop until every one is answered.
//

#ifndef ts_moveables_tests_client_harness_hpp
#define ts_moveables_tests_client_harness_hpp

#include "../test_helpers.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <optional>
#include <vector>

#include "http/client.hpp"
#include "http/server.hpp"
#include "net/url.hpp"

namespace client_tests {

using namespace std::chrono_literals;
using snicholls::event_loop;
using snicholls::http::client;
using snicholls::http::client_config;
using snicholls::http::client_errc;
using snicholls::http::client_request;
using snicholls::http::client_result;

// ------------------------------------------------------------------ helpers

// A server of our own on a thread
struct live_server {
    snicholls::http::server srv;
    std::uint16_t port = 0;
    explicit live_server(snicholls::http::server_config sc = {}) : srv(std::move(sc)) {}
    std::thread th;
    void start()
    {
        port = srv.listen("127.0.0.1", 0);
        th = std::thread([this] { srv.run(); });
        while (!srv.running()) std::this_thread::yield();
    }
    ~live_server()
    {
        srv.stop();
        if (th.joinable()) th.join();
    }
    std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port) + path; }
};

// A raw TCP server: each accepted connection is handed to `script` on a thread of its own
struct raw_server {
    int fd = -1;
    std::uint16_t port = 0;
    std::thread th;
    std::vector<std::thread> conns;
    std::atomic<bool> stop{false};
    std::atomic<int> accepted{0};

    explicit raw_server(std::function<void(int)> script)
    {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        const int on = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        assert(::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
        assert(::listen(fd, 16) == 0);
        socklen_t len = sizeof a;
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        th = std::thread([this, script] {
            for (;;) {
                const int c = ::accept(fd, nullptr, nullptr);
                if (c < 0 || stop.load()) {
                    if (c >= 0) ::close(c);
                    return;
                }
                ++accepted;
                conns.emplace_back([c, script] {
                    script(c);
                    ::close(c);
                });
            }
        });
    }
    ~raw_server()
    {
        stop = true;
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
        // Wake a blocked accept on platforms where closing the socket does not
        const int s = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
        ::close(s);
        th.join();
        for (auto& t : conns) t.join();
    }
    std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port) + path; }
};

// Read one request head (and a Content-Length body) from a raw connection; empty when the client went away
inline std::string read_request(int c)
{
    std::string got;
    char buf[4096];
    while (got.find("\r\n\r\n") == std::string::npos) {
        const ssize_t n = ::recv(c, buf, sizeof buf, 0);
        if (n <= 0) return {};
        got.append(buf, std::size_t(n));
    }
    const std::size_t cl = got.find("Content-Length: ");
    if (cl != std::string::npos) {
        const std::size_t want = std::stoul(got.substr(cl + 16));
        const std::size_t have = got.size() - (got.find("\r\n\r\n") + 4);
        for (std::size_t left = want - have; left > 0;) {
            const ssize_t n = ::recv(c, buf, sizeof buf, 0);
            if (n <= 0) break;
            got.append(buf, std::size_t(n));
            left -= std::min<std::size_t>(left, std::size_t(n));
        }
    }
    return got;
}

inline void send_all(int c, const std::string& s)
{
    std::size_t off = 0;
    while (off < s.size()) {
        const ssize_t n = ::send(c, s.data() + off, s.size() - off, 0);
        if (n <= 0) return;
        off += std::size_t(n);
    }
}

// Run requests on a loop on this thread until every callback has run (or 10 s pass)
struct driver {
    event_loop loop;
    client http;
    explicit driver(client_config cfg = {}) : http(loop, std::move(cfg)) {}
    std::vector<client_result> run(std::vector<client_request> reqs)
    {
        std::vector<std::optional<client_result>> out(reqs.size());
        std::size_t done = 0;
        for (std::size_t i = 0; i < reqs.size(); ++i)
            http.request(reqs[i], [&out, &done, i](client_result r) {
                out[i].emplace(std::move(r));
                ++done;
            });
        const auto until = std::chrono::steady_clock::now() + 10s;
        while (done < reqs.size() && std::chrono::steady_clock::now() < until) loop.run_once(10ms);
        assert(done == reqs.size());
        std::vector<client_result> results;
        for (auto& o : out) results.push_back(std::move(*o));
        return results;
    }
    client_result one(client_request r) { return std::move(run({std::move(r)})[0]); }
};

inline client_errc code_of(const client_result& r) { return r.unwrap_err_unchecked().code(); }

} // namespace client_tests

#endif // ts_moveables_tests_client_harness_hpp
