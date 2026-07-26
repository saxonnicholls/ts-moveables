//
//  http_scale.cpp
//  TSMoveables
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - how far does the HTTP server scale across cores?
//
//  Everything measured so far runs one event_loop on one thread. This sweeps
//  N independent reactors - N loops, N threads, all sharing one listening
//  port via SO_REUSEPORT so the kernel distributes accepts - and reports what
//  each step buys.
//
//  This is the multi-reactor (SO_REUSEPORT sharding) pattern: no shared state
//  between loops at all, no locks between them, each owning its own sessions
//  outright. It is available here because a fully configured server is a
//  moveable value, so building N of them is a loop rather than an
//  architecture.
//
//  It also prints the per-reactor connection counts, because "we started 32
//  loops" means nothing if the kernel handed every connection to one of them -
//  and SO_REUSEPORT load-balances on Linux but is not specified to on every
//  BSD. Measuring the distribution is the only way to know.
//
//  Honesty, up front: the load generator lives on this same box and competes
//  for the same cores. Past the point where client and server together exceed
//  the hardware, this measures the machine, not the server. The client cost
//  is reported alongside so that ceiling is visible rather than implied.
//
//  Build and run:  make bench-scale
//

#include "../TSMoveables/http_server.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if !SNICHOLLS_HAS_HTTP_SERVER

int main()
{
    std::printf("http scaling: POSIX only - skipped\n");
    return 0;
}

#else

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace snicholls;
using namespace std::chrono_literals;

namespace {

bool markdown = false;

double seconds_since(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

void die(const char* what)
{
    std::fprintf(stderr, "FAILED: %s\n", what);
    std::exit(1);
}

// ------------------------------------------------------------------- client

class conn {
public:
    bool open(std::uint16_t port)
    {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0)
            return false;
        linger lg{};
        lg.l_onoff = 1;
        lg.l_linger = 0;
        ::setsockopt(fd_, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
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
        tv.tv_sec = 20;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        return true;
    }

    conn() = default;
    conn(conn&& o) noexcept : fd_(o.fd_), buf_(std::move(o.buf_)) { o.fd_ = -1; }
    conn(const conn&) = delete;
    ~conn()
    {
        if (fd_ >= 0)
            ::close(fd_);
    }

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

    // Count complete responses in the stream. Bodies here are a fixed, known
    // size, so framing needs no parsing beyond the terminator
    int drain_responses(int want, const std::string& marker)
    {
        int got = 0;
        while (got < want) {
            std::size_t at;
            while ((at = buf_.find(marker)) != std::string::npos) {
                buf_.erase(0, at + marker.size());
                if (++got == want)
                    return got;
            }
            char tmp[65536];
            const ssize_t n = ::recv(fd_, tmp, sizeof tmp, 0);
            if (n <= 0)
                return got;
            buf_.append(tmp, std::size_t(n));
        }
        return got;
    }

private:
    int fd_ = -1;
    std::string buf_;
};

// ---------------------------------------------------------------- one sweep

struct sweep_result {
    int reactors = 0;
    double req_per_sec = 0;
    long long ok = 0;
    double seconds = 0;
    double cores_used = 0;              // CPU seconds burned per wall second
    std::size_t busiest = 0;            // connections landing on the busiest reactor
    std::size_t idle_reactors = 0;      // reactors the kernel never fed
};

sweep_result run_sweep(int reactors, int client_threads, int depth, int rounds,
                       int extra_headers)
{
    // N servers, N loops, N threads, one shared port. Nothing is shared
    // between them - not a queue, not a lock, not a session table.
    std::vector<std::unique_ptr<http::server>> servers;
    std::vector<std::thread> loops;
    std::uint16_t port = 0;

    for (int i = 0; i < reactors; ++i) {
        auto srv = std::unique_ptr<http::server>(new http::server());
        srv->get("/ping", [](const http::request&, http::responder r) {
            r.send(200, "text/plain", "pong");
        });
        // One listener, shared by every reactor. SO_REUSEPORT would be the
        // better route on Linux (the kernel balances accepts), but it does not
        // balance on macOS/BSD, so a shared listener is what works everywhere
        const std::uint16_t got = (i == 0) ? srv->listen("127.0.0.1", 0)
                                           : srv->listen_shared(servers.front()->listener());
        if (i == 0)
            port = got;
        else if (got != port)
            die("shared listener: reactors did not share a port");
        servers.push_back(std::move(srv));
    }
    for (auto& s : servers)
        loops.emplace_back([&s] { s->run(); });
    for (auto& s : servers)
        while (!s->running())
            std::this_thread::yield();

    // Pipelined load: each client connection keeps `depth` requests in flight,
    // so the measurement is not bounded by loopback round trips
    // Realistic requests carry a dozen headers, not one. Header count is the
    // parser's actual workload, so it is a knob here rather than an assumption
    std::string headers = "Host: scale\r\n";
    for (int h = 0; h < extra_headers; ++h)
        headers += "X-Header-" + std::to_string(h) +
                   ": some-plausible-value-" + std::to_string(h) + "\r\n";
    std::string batch;
    for (int i = 0; i < depth; ++i)
        batch += "GET /ping HTTP/1.1\r\n" + headers + "\r\n";
    const std::string marker = "\r\n\r\npong";

    std::atomic<long long> ok{0};
    std::vector<std::thread> clients;
    clients.reserve(std::size_t(client_threads));

    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    for (int t = 0; t < client_threads; ++t)
        clients.emplace_back([&] {
            conn c;
            if (!c.open(port)) {
                ready.fetch_add(1);
                return;
            }
            c.send_raw(batch);                  // warm the connection
            c.drain_responses(depth, marker);
            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();

            long long mine = 0;
            for (int r = 0; r < rounds; ++r) {
                if (!c.send_raw(batch))
                    break;
                const int got = c.drain_responses(depth, marker);
                mine += got;
                if (got != depth)
                    break;
            }
            ok.fetch_add(mine, std::memory_order_relaxed);
        });

    while (ready.load() < client_threads)
        std::this_thread::yield();

    // CPU accounting decides the central question: is this measuring the
    // server, or a box that has run out of cores? Client and server share this
    // process, so the total is what matters against the core count
    auto cpu_seconds = [] {
        rusage ru{};
        ::getrusage(RUSAGE_SELF, &ru);
        return double(ru.ru_utime.tv_sec) + double(ru.ru_utime.tv_usec) / 1e6 +
               double(ru.ru_stime.tv_sec) + double(ru.ru_stime.tv_usec) / 1e6;
    };
    const double cpu0 = cpu_seconds();
    const auto t0 = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& t : clients)
        t.join();
    const double secs = seconds_since(t0);
    const double cpu_used = cpu_seconds() - cpu0;

    sweep_result out;
    out.reactors = reactors;
    out.seconds = secs;
    out.ok = ok.load();
    out.req_per_sec = double(out.ok) / secs;
    out.cores_used = cpu_used / secs;
    for (const auto& s : servers) {
        const std::size_t n = std::size_t(s->total_connections());
        out.busiest = std::max(out.busiest, n);
        if (n == 0)
            ++out.idle_reactors;
    }

    for (auto& s : servers)
        s->stop();
    for (auto& t : loops)
        t.join();
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    int max_reactors = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--markdown") == 0)
            markdown = true;
        else if (std::strcmp(argv[i], "--max") == 0 && i + 1 < argc)
            max_reactors = std::atoi(argv[++i]);
    }

    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    if (max_reactors <= 0)
        max_reactors = int(hw);

    // Overridable, because the first question this benchmark has to answer is
    // whether it is measuring the server or its own load generator
    int client_threads = 32;
    int depth = 32;
    int rounds = 400;
    int extra_headers = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--clients") == 0 && i + 1 < argc)
            client_threads = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--depth") == 0 && i + 1 < argc)
            depth = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--rounds") == 0 && i + 1 < argc)
            rounds = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--headers") == 0 && i + 1 < argc)
            extra_headers = std::atoi(argv[++i]);
    }

    std::vector<int> steps;
    for (int n = 1; n <= max_reactors; n *= 2)
        steps.push_back(n);
    if (steps.back() != max_reactors)
        steps.push_back(max_reactors);

    if (markdown)
        std::printf("### `make bench-scale` - multi-reactor scaling\n\n"
                    "| Reactors (threads) | Throughput | vs 1 reactor | CPU in use | "
                    "Accept distribution |\n|---|---|---|---|---|\n");
    else
        std::printf("http multi-reactor scaling  (%u hw threads, %d client threads, "
                    "pipeline depth %d)\n\n", hw, client_threads, depth);

    double base = 0;
    for (int n : steps) {
        std::fprintf(stderr, "  ... %d reactors\n", n);
        std::fflush(stderr);
        const auto r = run_sweep(n, client_threads, depth, rounds, extra_headers);
        if (base == 0)
            base = r.req_per_sec;

        char dist[96];
        if (r.reactors == 1)
            std::snprintf(dist, sizeof dist, "-");
        else if (r.idle_reactors)
            std::snprintf(dist, sizeof dist, "**%zu of %d reactors never fed**",
                          r.idle_reactors, r.reactors);
        else
            std::snprintf(dist, sizeof dist, "all %d fed, busiest %zu conns",
                          r.reactors, r.busiest);

        if (markdown)
            std::printf("| %d | %.0f req/s | %.2fx | %.1f of %u cores busy | %s |\n",
                        r.reactors, r.req_per_sec, r.req_per_sec / base, r.cores_used, hw, dist);
        else
            std::printf("  %-3d reactors  %10.0f req/s   %5.2fx   %5.1f/%u cores   %s\n",
                        r.reactors, r.req_per_sec, r.req_per_sec / base, r.cores_used, hw, dist);
        std::this_thread::sleep_for(300ms);
    }

    if (markdown)
        std::printf("\nThe load generator runs on this same box and competes for the same "
                    "cores, so the top of this table measures the machine as much as the "
                    "server.\n");
    else
        std::printf("\nNote: the load generator shares this box - the top of the sweep "
                    "measures the machine as much as the server.\n");
    return 0;
}

#endif
