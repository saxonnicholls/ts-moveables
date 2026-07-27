//
//  http_compare.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - head to head against cpp-httplib
//
//  cpp-httplib (yhirose, MIT) is the reference for "drop in one header and it
//  works", and it earned that. This runs the SAME load generator against it
//  and against snicholls::http::server, so the architectural difference -
//  blocking thread-per-connection versus a non-blocking reactor - shows up as
//  numbers instead of adjectives.
//
//  Fairness, deliberately:
//    - identical routes, identical response bodies, identical client code
//    - httplib is measured twice: exactly as it ships, and tuned with a large
//      thread pool and a raised keep-alive count, which is the best case its
//      design allows
//    - its listen backlog is raised to match ours (it ships with 5), because
//      losing on a backlog constant would say nothing about the design
//    - both servers are built -O3 -DNDEBUG in this one binary, run one at a
//      time, on the same machine, in the same conditions
//
//  Not fair to read too much into: the load generator shares the machine with
//  the server, and loopback is not a network. These are single-box numbers.
//
//  Build and run:  make bench-http     (fetches httplib into third_party/)
//

#include "../TSMoveables/http/server.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if !SNICHOLLS_HAS_HTTP_SERVER

int main()
{
    std::printf("http comparison: POSIX only - skipped\n");
    return 0;
}

#else

#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <netinet/tcp.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace snicholls;
using namespace std::chrono_literals;

namespace {

bool markdown = false;

void note(const char* what)
{
    std::fprintf(stderr, "  ... %s\n", what);
    std::fflush(stderr);
}

double seconds_since(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

std::size_t descriptor_limit()
{
    rlimit rl{};
    if (::getrlimit(RLIMIT_NOFILE, &rl) != 0)
        return 256;
    return std::size_t(rl.rlim_cur);
}

const std::string kPayload(1024, 'p');

// Every held-open scenario gets the same wall-clock budget. A server that
// cannot serve the set inside it reports how far it got, which is the
// honest answer rather than an unbounded wait
const double kBudget = 20.0;

// ------------------------------------------------------------------- client

class conn {
public:
    bool open(std::uint16_t port, bool reset_on_close = true, int timeout_sec = 15,
              int connect_timeout_sec = 3)
    {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0)
            return false;
        if (reset_on_close) {
            // RST rather than a graceful close, so churning thousands of
            // connections does not exhaust the ephemeral port range
            linger lg{};
            lg.l_onoff = 1;
            lg.l_linger = 0;
            ::setsockopt(fd_, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
        }
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        // Non-blocking connect with a deadline. When a server's accept queue
        // backs up, the kernel drops SYNs and a blocking connect() can stall
        // for many seconds - which would measure the harness, not the server
        const int flags = ::fcntl(fd_, F_GETFL, 0);
        ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
        int rc = ::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a);
        if (rc != 0 && errno == EINPROGRESS) {
            // poll(), never select(): FD_SETSIZE is 1024, and with both client
            // and server sockets in this one process the descriptor numbers
            // cross that at around 500 connections. select() then fails
            // silently and every server appears to plateau in the same place -
            // a measurement artefact that looks exactly like a real limit
            pollfd pfd{};
            pfd.fd = fd_;
            pfd.events = POLLOUT;
            rc = (::poll(&pfd, 1, connect_timeout_sec * 1000) > 0) ? 0 : -1;
            if (rc == 0) {
                int err = 0;
                socklen_t len = sizeof err;
                ::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len);
                rc = err == 0 ? 0 : -1;
            }
        }
        ::fcntl(fd_, F_SETFL, flags);
        if (rc != 0) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        const int on = 1;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
        timeval tv{};
        tv.tv_sec = timeout_sec;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        return true;
    }

    conn() = default;
    conn(conn&& o) noexcept : fd_(o.fd_), buf_(std::move(o.buf_)) { o.fd_ = -1; }
    conn(const conn&) = delete;
    conn& operator=(const conn&) = delete;
    ~conn() { close(); }

    void close()
    {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    bool valid() const noexcept { return fd_ >= 0; }

    bool send_raw(const std::string& s)
    {
        std::size_t off = 0;
        while (off < s.size()) {
            const ssize_t n = ::send(fd_, s.data() + off, s.size() - off, 0);
            if (n <= 0)
                return false;
            off += std::size_t(n);
        }
        return true;
    }

    enum class step { ready, again, dead };

    // One recv attempt against a short socket timeout. Buffered bytes are
    // kept, so a response split across attempts still completes - this is
    // what lets the harness bound its own runtime instead of blocking on a
    // connection whose server has not got to it yet.
    step try_read_response(std::string& body, int& status)
    {
        for (;;) {
            const std::size_t hend = buf_.find("\r\n\r\n");
            if (hend != std::string::npos) {
                status = std::atoi(buf_.c_str() + 9);
                std::size_t want = 0;
                const std::size_t cl = find_ci(buf_, "\r\ncontent-length:", hend);
                if (cl != std::string::npos)
                    want = std::size_t(std::atoll(buf_.c_str() + cl + 17));
                if (buf_.size() >= hend + 4 + want) {
                    body.assign(buf_, hend + 4, want);
                    buf_.erase(0, hend + 4 + want);
                    return step::ready;
                }
            }
            char tmp[16384];
            const ssize_t n = ::recv(fd_, tmp, sizeof tmp, 0);
            if (n > 0) {
                buf_.append(tmp, std::size_t(n));
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return step::again;
            return step::dead;
        }
    }

    void set_recv_timeout_ms(int ms)
    {
        timeval tv{};
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }

    bool read_response(std::string& body, int& status)
    {
        for (;;) {
            const std::size_t hend = buf_.find("\r\n\r\n");
            if (hend != std::string::npos) {
                status = std::atoi(buf_.c_str() + 9);
                std::size_t want = 0;
                const std::size_t cl = find_ci(buf_, "\r\ncontent-length:", hend);
                if (cl != std::string::npos)
                    want = std::size_t(std::atoll(buf_.c_str() + cl + 17));
                if (buf_.size() >= hend + 4 + want) {
                    body.assign(buf_, hend + 4, want);
                    buf_.erase(0, hend + 4 + want);
                    return true;
                }
            }
            char tmp[16384];
            const ssize_t n = ::recv(fd_, tmp, sizeof tmp, 0);
            if (n <= 0)
                return false;
            buf_.append(tmp, std::size_t(n));
        }
    }

private:
    static std::size_t find_ci(const std::string& h, const char* needle, std::size_t limit)
    {
        const std::size_t nn = std::strlen(needle);
        for (std::size_t i = 0; i + nn <= limit; ++i) {
            std::size_t k = 0;
            while (k < nn) {
                char a = h[i + k];
                if (a >= 'A' && a <= 'Z')
                    a = char(a - 'A' + 'a');
                if (a != needle[k])
                    break;
                ++k;
            }
            if (k == nn)
                return i;
        }
        return std::string::npos;
    }

    int fd_ = -1;
    std::string buf_;
};

// ------------------------------------------------------------- the scenarios

struct result {
    double req_per_sec = 0;
    double seconds = 0;
    long long ok = 0;
    long long attempted = 0;
    std::size_t served = 0;
    std::size_t held = 0;
    long long reconnects = 0;
    double open_seconds = 0;
    bool timed_out = false;
};

// Keep-alive request/response over `threads` concurrent connections. Each
// connection is synchronous, so this measures the server's ability to keep
// many conversations moving at once.
result bench_keepalive(std::uint16_t port, int threads, int per_thread, const char* target,
                       std::size_t expect_size, int warmup_per_thread = 500)
{
    std::atomic<long long> ok{0};
    std::atomic<long long> reconnects{0};
    std::atomic<bool> timing{false};

    // Every server is asked for the SAME number of requests. A server that
    // hangs up on a keep-alive connection (httplib closes after its
    // keep_alive_max_count) simply gets reconnected to, and pays the real
    // cost of that in the rate - which is what a real client would see,
    // rather than a short sample that measures startup noise
    auto run_one = [&](int count, bool counting) {
        conn c;
        if (!c.open(port))
            return;
        const std::string req =
            std::string("GET ") + target + " HTTP/1.1\r\nHost: bench\r\n\r\n";
        std::string body;
        int status = 0;
        for (int i = 0; i < count; ++i) {
            if (!c.send_raw(req) || !c.read_response(body, status)) {
                c.close();
                if (!c.open(port))
                    return;                     // cannot reconnect: stop, reported as a shortfall
                if (counting)
                    reconnects.fetch_add(1, std::memory_order_relaxed);
                --i;
                continue;
            }
            if (counting && status == 200 && body.size() == expect_size)
                ok.fetch_add(1, std::memory_order_relaxed);
        }
    };

    // Warm up: thread pools spin up, buffers reach steady state, caches fill
    {
        std::vector<std::thread> w;
        w.reserve(std::size_t(threads));
        for (int t = 0; t < threads; ++t)
            w.emplace_back([&] { run_one(warmup_per_thread, false); });
        for (auto& t : w)
            t.join();
    }

    timing.store(true);
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> clients;
    clients.reserve(std::size_t(threads));
    for (int t = 0; t < threads; ++t)
        clients.emplace_back([&] { run_one(per_thread, true); });
    for (auto& t : clients)
        t.join();

    result r;
    r.seconds = seconds_since(t0);
    r.ok = ok.load();
    r.attempted = static_cast<long long>(threads) * per_thread;
    r.req_per_sec = double(r.ok) / r.seconds;
    r.reconnects = reconnects.load();
    return r;
}

// The C10K shape: open `target_conns` connections and hold them all open at
// once, then serve one request on each. This is where a thread-per-connection
// design runs out of threads and a reactor does not notice.
result bench_concurrent(std::uint16_t port, std::size_t target_conns, double budget_seconds)
{
    result r;
    std::vector<conn> conns;
    conns.reserve(target_conns);
    const auto open_t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < target_conns; ++i) {
        conn c;
        if (!c.open(port, true, 5, 3))
            break;                              // refused or timed out: the server cannot take more
        conns.push_back(std::move(c));
        if (seconds_since(open_t0) > budget_seconds)
            break;                              // establishing them is itself taking too long
    }
    r.held = conns.size();
    r.attempted = static_cast<long long>(target_conns);
    r.open_seconds = seconds_since(open_t0);
    for (auto& c : conns)
        c.set_recv_timeout_ms(20);              // short: the harness paces itself

    const auto t0 = std::chrono::steady_clock::now();
    const std::string req = "GET /ping HTTP/1.1\r\nHost: bench\r\n\r\n";
    for (auto& c : conns)
        if (!c.send_raw(req))
            break;

    // Sweep the whole set repeatedly until every connection has answered or
    // the budget runs out. No single slow connection can stall the measurement
    std::vector<char> done(conns.size(), 0);
    while (r.served < conns.size() && seconds_since(t0) < budget_seconds) {
        bool progress = false;
        for (std::size_t i = 0; i < conns.size(); ++i) {
            if (done[i])
                continue;
            std::string body;
            int status = 0;
            const auto st = conns[i].try_read_response(body, status);
            if (st == conn::step::ready) {
                done[i] = 1;
                progress = true;
                if (status == 200 && body == "pong")
                    ++r.served;
            } else if (st == conn::step::dead) {
                done[i] = 1;                    // counted as unserved
                progress = true;
            }
            if (seconds_since(t0) >= budget_seconds)
                break;
        }
        if (!progress)
            std::this_thread::sleep_for(5ms);
    }
    r.timed_out = (r.served < conns.size());
    r.seconds = seconds_since(t0);
    conns.clear();
    return r;
}

// ------------------------------------------------------------------ reporting

struct row {
    std::string scenario;
    std::string ours;
    std::string theirs_default;
    std::string theirs_tuned;
};
std::vector<row> rows;

std::string fmt_rate(const result& r)
{
    char b[128];
    if (r.ok < r.attempted)
        std::snprintf(b, sizeof b, "%.0f req/s (only %lld of %lld completed)",
                      r.req_per_sec, r.ok, r.attempted);
    else if (r.reconnects)
        std::snprintf(b, sizeof b, "%.0f req/s (+%lld reconnects)", r.req_per_sec, r.reconnects);
    else
        std::snprintf(b, sizeof b, "%.0f req/s", r.req_per_sec);
    return b;
}

std::string fmt_conc(const result& r)
{
    char b[192];
    if (r.held < std::size_t(r.attempted))
        std::snprintf(b, sizeof b,
                      "only %zu of %lld could connect (%.1f s); %zu served in %.1f s",
                      r.held, r.attempted, r.open_seconds, r.served, r.seconds);
    else if (r.timed_out)
        std::snprintf(b, sizeof b, "%zu of %zu served, rest unserved at %.0f s budget",
                      r.served, r.held, r.seconds);
    else
        std::snprintf(b, sizeof b, "all %zu served in %.0f ms", r.served, r.seconds * 1000.0);
    return b;
}

} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--markdown") == 0)
            markdown = true;

    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const std::size_t fdlimit = descriptor_limit();
    // Two descriptors per connection in this single process, plus headroom
    const std::size_t big_conns = (fdlimit > 4096) ? std::min<std::size_t>(10000, (fdlimit - 512) / 2)
                                                   : 512;
    const int tuned_threads = 1024;

    // ------------------------------------------------------------ ts-moveables
    result ours_ka, ours_ka_hi, ours_payload, ours_c1k, ours_big;
    {
        http::server srv;
        srv.get("/ping", [](const http::request&, http::responder r) {
            r.send(200, "text/plain", "pong");
        });
        srv.get("/payload", [](const http::request&, http::responder r) {
            r.send(200, "application/octet-stream", kPayload);
        });
        const std::uint16_t port = srv.listen("127.0.0.1", 0);
        std::thread th([&] { srv.run(); });
        while (!srv.running())
            std::this_thread::yield();

        note("ts-moveables: keep-alive 16");
        ours_ka      = bench_keepalive(port, 16, 5000, "/ping", 4);
        note("ts-moveables: keep-alive 256");
        ours_ka_hi   = bench_keepalive(port, 256, 400, "/ping", 4);
        note("ts-moveables: payload");
        ours_payload = bench_keepalive(port, 16, 4000, "/payload", 1024);
        note("ts-moveables: 1k concurrent");
        ours_c1k     = bench_concurrent(port, 1000, kBudget);
        note("ts-moveables: big concurrent");
        ours_big     = bench_concurrent(port, big_conns, kBudget);

        srv.stop();
        th.join();
    }

    // ----------------------------------------------------------------- httplib
    auto run_httplib = [&](bool tuned) {
        struct { result ka, ka_hi, payload, c1k, big; } out;
        httplib::Server svr;
        svr.Get("/ping", [](const httplib::Request&, httplib::Response& res) {
            res.set_content("pong", "text/plain");
        });
        svr.Get("/payload", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(kPayload, "application/octet-stream");
        });
        svr.set_tcp_nodelay(true);
        if (tuned) {
            // The best case its design allows: a large pool, and keep-alive
            // counts high enough that it never hangs up mid-measurement
            svr.new_task_queue = [] { return new httplib::ThreadPool(tuned_threads, tuned_threads); };
            svr.set_keep_alive_max_count(1000000);
            svr.set_keep_alive_timeout(5);
        }

        const int port = svr.bind_to_any_port("127.0.0.1");
        std::thread th([&] { svr.listen_after_bind(); });
        while (!svr.is_running())
            std::this_thread::sleep_for(5ms);

        const std::uint16_t p = static_cast<std::uint16_t>(port);
        const int per = tuned ? 5000 : 100;
        note(tuned ? "httplib tuned: keep-alive 16" : "httplib default: keep-alive 16");
        out.ka      = bench_keepalive(p, 16, per, "/ping", 4);
        note("httplib: keep-alive 256");
        out.ka_hi   = bench_keepalive(p, 256, tuned ? 400 : 100, "/ping", 4);
        note("httplib: payload");
        out.payload = bench_keepalive(p, 16, tuned ? 4000 : 100, "/payload", 1024);
        note("httplib: 1k concurrent");
        out.c1k     = bench_concurrent(p, 1000, kBudget);
        std::this_thread::sleep_for(1s);
        note("httplib: big concurrent");
        out.big     = bench_concurrent(p, big_conns, kBudget);

        svr.stop();
        th.join();
        return out;
    };

    const auto theirs_default = run_httplib(false);
    std::this_thread::sleep_for(2s);            // let the previous server's sockets drain
    const auto theirs_tuned = run_httplib(true);

    // ------------------------------------------------------------------ report
    rows.push_back({"keep-alive, 16 connections",
                    fmt_rate(ours_ka), fmt_rate(theirs_default.ka), fmt_rate(theirs_tuned.ka)});
    rows.push_back({"keep-alive, 256 connections",
                    fmt_rate(ours_ka_hi), fmt_rate(theirs_default.ka_hi), fmt_rate(theirs_tuned.ka_hi)});
    rows.push_back({"1 KiB payloads, 16 connections",
                    fmt_rate(ours_payload), fmt_rate(theirs_default.payload),
                    fmt_rate(theirs_tuned.payload)});
    rows.push_back({"1,000 held open at once",
                    fmt_conc(ours_c1k), fmt_conc(theirs_default.c1k), fmt_conc(theirs_tuned.c1k)});
    {
        char label[64];
        std::snprintf(label, sizeof label, "%zu held open at once", big_conns);
        rows.push_back({label, fmt_conc(ours_big), fmt_conc(theirs_default.big),
                        fmt_conc(theirs_tuned.big)});
    }

    if (markdown) {
        std::printf("### `make bench-http` - head to head vs cpp-httplib\n\n");
        std::printf("| Scenario | ts-moveables (1 thread) | cpp-httplib (as shipped) | "
                    "cpp-httplib (tuned, %d threads) |\n|---|---|---|---|\n", tuned_threads);
        for (const auto& r : rows)
            std::printf("| %s | %s | %s | %s |\n", r.scenario.c_str(), r.ours.c_str(),
                        r.theirs_default.c_str(), r.theirs_tuned.c_str());
        std::printf("\n%u hardware threads, descriptor limit %zu. Single box, loopback: "
                    "the load generator shares the machine with the server.\n", hw, fdlimit);
    } else {
        std::printf("head to head vs cpp-httplib  (%u hw threads, fd limit %zu)\n\n", hw, fdlimit);
        std::printf("  %-32s %-38s %-38s %s\n", "scenario", "ts-moveables (1 thread)",
                    "httplib (as shipped)", "httplib (tuned)");
        for (const auto& r : rows)
            std::printf("  %-32s %-38s %-38s %s\n", r.scenario.c_str(), r.ours.c_str(),
                        r.theirs_default.c_str(), r.theirs_tuned.c_str());
        std::printf("\nSingle box, loopback - the load generator shares the machine.\n");
    }
    return 0;
}

#endif
