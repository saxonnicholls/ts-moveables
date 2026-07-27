//
//  http_server_demo.cpp
//  TSMoveables
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - the HTTP server, measured
//
//  A working server and a load generator in one program, so the numbers come
//  with the code that produced them. It measures what a reactor is actually
//  for:
//
//    1. throughput   - keep-alive request/response rate through one loop
//    2. concurrency  - many simultaneous connections held open at once, which
//                      is where a thread-per-connection design runs out of
//                      threads and a reactor does not notice
//    3. async        - handlers that answer later, off the loop thread, with
//                      no thread parked per in-flight request
//
//  Every response is verified, so this is an integration test that happens to
//  print a table. It exits non-zero if anything is wrong.
//
//  Build and run:   make demo-http              (compiled -O3 -DNDEBUG)
//
//  Honesty notes. The load generator shares the machine with the server, so
//  these are single-box numbers - fine for tracking regressions and for
//  comparing designs on identical hardware, not a substitute for the phase-6
//  head-to-head against cpp-httplib across a real network. The connection
//  count is clamped to the process descriptor limit, and the limit is printed.
//

#include "../TSMoveables/http/server.hpp"
#include "../TSMoveables/moveable/mutex.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if SNICHOLLS_HAS_HTTP_SERVER

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace snicholls;
using namespace snicholls::http;
using namespace std::chrono_literals;

namespace {

bool markdown = false;

void check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::exit(1);
    }
}

void row(const char* what, const char* result)
{
    if (markdown)
        std::printf("| %s | %s |\n", what, result);
    else
        std::printf("  %-42s %s\n", what, result);
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

// A blocking keep-alive client, deliberately simple
class conn {
public:
    // reset_on_close: close with RST instead of a graceful FIN, so the
    // client side never enters TIME_WAIT. A load generator that churns ten
    // thousand connections would otherwise exhaust the ephemeral port range
    // and poison whatever runs next - which is exactly what real load
    // generators set this for, not a trick to flatter the numbers.
    bool open(std::uint16_t port, bool reset_on_close = false)
    {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0)
            return false;
        if (reset_on_close) {
            linger lg{};
            lg.l_onoff = 1;
            lg.l_linger = 0;
            ::setsockopt(fd_, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
        }
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        const int on = 1;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
        timeval tv{};
        tv.tv_sec = 10;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        return true;
    }

    ~conn() { close(); }
    conn() = default;
    conn(conn&& o) noexcept : fd_(o.fd_), buf_(std::move(o.buf_)) { o.fd_ = -1; }
    conn(const conn&) = delete;
    conn& operator=(const conn&) = delete;

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

    // Read one response; returns the body, or false on failure
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
                char a = h[i + k], b = needle[k];
                if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
                if (a != b)
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

} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--markdown") == 0)
            markdown = true;

    if (markdown)
        std::printf("### `make demo-http` - non-blocking HTTP/1.1 server\n\n"
                    "| Scenario | Result |\n|---|---|\n");
    else
        std::printf("http server demo - a reactor under load\n\n");

    char buf[192];
    const std::string payload(1024, 'p');       // a 1 KiB response body

    server srv;
    std::atomic<long long> handled{0};
    std::vector<std::thread> workers;
    moveable_mutex<> worker_lock;

    srv.get("/ping", [&](const request&, responder r) {
        handled.fetch_add(1, std::memory_order_relaxed);
        r.send(200, "text/plain", "pong");
    });
    srv.get("/payload", [&](const request&, responder r) {
        handled.fetch_add(1, std::memory_order_relaxed);
        r.send(200, "application/octet-stream", payload);
    });
    srv.post("/echo", [&](const request& req, responder r) {
        handled.fetch_add(1, std::memory_order_relaxed);
        r.send(200, "application/octet-stream", req.body);
    });
    // The async route: the handler returns immediately and the response is
    // completed later, from a thread that is not the loop thread. No thread is
    // parked per in-flight request - that is the whole point.
    srv.get("/async", [&](const request&, responder r) {
        handled.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<moveable_mutex<>> g(worker_lock);
        workers.emplace_back([r = std::move(r)]() mutable {
            std::this_thread::sleep_for(5ms);
            r.send(200, "text/plain", "async");
        });
    });

    const std::uint16_t port = srv.listen("127.0.0.1", 0);
    std::thread loop_thread([&] { srv.run(); });
    while (!srv.running())
        std::this_thread::yield();

    // ------------------------------------------------- 1. keep-alive throughput
    // One connection per client thread, many requests down each. Note what
    // this really measures: with a synchronous client, each connection is
    // round-trip bound, so a handful of connections measures latency rather
    // than server capacity. Enough of them in parallel, and the loop becomes
    // the limit - which is the number worth quoting. The pipelined row below
    // removes the round trip entirely.
    {
        const int threads = 16;
        const int per_thread = 6000;
        std::atomic<long long> ok{0};

        const auto t0 = std::chrono::steady_clock::now();
        std::vector<std::thread> clients;
        for (int t = 0; t < threads; ++t)
            clients.emplace_back([&] {
                conn c;
                if (!c.open(port))
                    return;
                const std::string req = "GET /ping HTTP/1.1\r\nHost: bench\r\n\r\n";
                std::string body;
                int status = 0;
                for (int i = 0; i < per_thread; ++i) {
                    if (!c.send_raw(req) || !c.read_response(body, status))
                        return;
                    if (status == 200 && body == "pong")
                        ok.fetch_add(1, std::memory_order_relaxed);
                }
            });
        for (auto& t : clients)
            t.join();
        const double secs = seconds_since(t0);

        check(ok.load() == threads * per_thread, "every keep-alive request answered correctly");
        std::snprintf(buf, sizeof buf, "%lld req in %.2f s = %.0f req/s over %d connections",
                      ok.load(), secs, double(ok.load()) / secs, threads);
        row("keep-alive throughput, 4-byte bodies", buf);
    }

    // ------------------------------------------------------- 2. payload bandwidth
    {
        const int threads = 8;
        const int per_thread = 6000;
        std::atomic<long long> ok{0};

        const auto t0 = std::chrono::steady_clock::now();
        std::vector<std::thread> clients;
        for (int t = 0; t < threads; ++t)
            clients.emplace_back([&] {
                conn c;
                if (!c.open(port))
                    return;
                const std::string req = "GET /payload HTTP/1.1\r\nHost: bench\r\n\r\n";
                std::string body;
                int status = 0;
                for (int i = 0; i < per_thread; ++i) {
                    if (!c.send_raw(req) || !c.read_response(body, status))
                        return;
                    if (status == 200 && body.size() == 1024)
                        ok.fetch_add(1, std::memory_order_relaxed);
                }
            });
        for (auto& t : clients)
            t.join();
        const double secs = seconds_since(t0);

        check(ok.load() == threads * per_thread, "every 1 KiB response arrived intact");
        const double mib = double(ok.load()) * 1024.0 / (1024.0 * 1024.0);
        std::snprintf(buf, sizeof buf, "%.0f req/s, %.0f MiB/s of response body",
                      double(ok.load()) / secs, mib / secs);
        row("1 KiB payloads", buf);
    }

    // --------------------------------------------------- 3. pipelined requests
    // The reactor's best case: many requests in flight per round trip
    {
        const int depth = 64;
        const int rounds = 2000;
        conn c;
        check(c.open(port), "pipeline connection opened");
        std::string batch;
        for (int i = 0; i < depth; ++i)
            batch += "GET /ping HTTP/1.1\r\nHost: bench\r\n\r\n";

        long long ok = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int r = 0; r < rounds; ++r) {
            check(c.send_raw(batch), "pipeline batch sent");
            for (int i = 0; i < depth; ++i) {
                std::string body;
                int status = 0;
                check(c.read_response(body, status), "pipelined response read");
                if (status == 200 && body == "pong")
                    ++ok;
            }
        }
        const double secs = seconds_since(t0);
        check(ok == depth * rounds, "every pipelined response correct and in order");
        std::snprintf(buf, sizeof buf, "%lld req in %.2f s = %.0f req/s (depth %d)",
                      ok, secs, double(ok) / secs, depth);
        row("pipelined on one connection", buf);
    }

    // ------------------------------------------- 4. simultaneous connections
    // The C10K shape: hold many connections open at once and serve on all of
    // them. A thread-per-connection server needs a thread each; the reactor
    // needs one loop. Clamped to the descriptor limit, which is printed.
    {
        const std::size_t limit = descriptor_limit();
        const std::size_t want = 10000;
        // Leave headroom for the server's own accepted fds (2 per connection
        // in this single process), the listener, and stdio
        const std::size_t target = (limit > 64) ? std::min(want, (limit - 64) / 2) : 16;

        std::vector<conn> conns;
        conns.reserve(target);
        for (std::size_t i = 0; i < target; ++i) {
            conn c;
            if (!c.open(port, /*reset_on_close*/ true))
                break;
            conns.push_back(std::move(c));
        }
        const std::size_t held = conns.size();
        check(held > 0, "at least one connection held open");

        // Every held connection now does a request, in order
        const auto t0 = std::chrono::steady_clock::now();
        std::size_t served = 0;
        const std::string req = "GET /ping HTTP/1.1\r\nHost: bench\r\n\r\n";
        for (auto& c : conns)
            if (!c.send_raw(req))
                break;
        for (auto& c : conns) {
            std::string body;
            int status = 0;
            if (c.read_response(body, status) && status == 200 && body == "pong")
                ++served;
        }
        const double secs = seconds_since(t0);
        check(served == held, "every simultaneously-open connection was served");

        std::snprintf(buf, sizeof buf,
                      "%zu held open and all served in %.0f ms (fd limit %zu%s)",
                      held, secs * 1000.0, limit,
                      held < want ? ", clamped" : "");
        row("simultaneous connections", buf);
        conns.clear();
    }

    // ------------------------------------------------------ 5. async responders
    // 200 in-flight requests answered from worker threads, with the loop free
    // the whole time
    {
        const int clients_n = 200;
        std::atomic<int> ok{0};
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<std::thread> clients;
        clients.reserve(clients_n);
        for (int i = 0; i < clients_n; ++i)
            clients.emplace_back([&] {
                conn c;
                if (!c.open(port, /*reset_on_close*/ true))
                    return;
                if (!c.send_raw("GET /async HTTP/1.1\r\nHost: bench\r\nConnection: close\r\n\r\n"))
                    return;
                std::string body;
                int status = 0;
                if (c.read_response(body, status) && status == 200 && body == "async")
                    ok.fetch_add(1);
            });
        for (auto& t : clients)
            t.join();
        const double secs = seconds_since(t0);
        check(ok.load() == clients_n, "every async response was delivered");
        std::snprintf(buf, sizeof buf, "%d in flight, all answered off-loop in %.0f ms",
                      clients_n, secs * 1000.0);
        row("async responders", buf);
    }

    // ------------------------------------------------------- 6. large bodies
    {
        const std::string big(2u * 1024 * 1024, 'z');
        conn c;
        check(c.open(port), "large-body connection opened");
        std::string req = "POST /echo HTTP/1.1\r\nHost: bench\r\nContent-Length: " +
                          std::to_string(big.size()) + "\r\n\r\n";
        req += big;
        const auto t0 = std::chrono::steady_clock::now();
        check(c.send_raw(req), "large body sent");
        std::string body;
        int status = 0;
        check(c.read_response(body, status), "large body echoed");
        const double secs = seconds_since(t0);
        check(status == 200 && body == big, "2 MiB round-tripped byte for byte");
        std::snprintf(buf, sizeof buf, "2 MiB up and back in %.0f ms (%.0f MiB/s round trip)",
                      secs * 1000.0, 4.0 / secs);
        row("large request and response bodies", buf);
    }

    {
        std::lock_guard<moveable_mutex<>> g(worker_lock);
        for (auto& t : workers)
            t.join();
    }

    std::snprintf(buf, sizeof buf, "%llu connections, %llu requests, zero errors",
                  (unsigned long long)srv.total_connections(),
                  (unsigned long long)srv.total_requests());
    row("server totals", buf);
    row("server threads", "1 - a single event_loop served all of the above");

    srv.stop();
    loop_thread.join();

    check(handled.load() > 0, "handlers ran");
    if (!markdown)
        std::printf("\nall http server demo checks passed\n");
    return 0;
}

#else // !SNICHOLLS_HAS_HTTP_SERVER

int main()
{
    std::printf("http server demo: the server is POSIX-only in phase 1 - demo skipped\n");
    return 0;
}

#endif
