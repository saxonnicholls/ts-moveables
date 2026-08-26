//
//  rpc_latency.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - round-trip latency for small binary RPC.
//
//  Everything else here measures throughput. This measures the other thing, and
//  they are not convertible: a stream that sustains 180k messages/s with a
//  megabyte in flight says nothing about how long one request waits for its
//  answer. Inverting a throughput figure into an RTT is the single easiest way
//  to be badly wrong about this stack, so this exists to stop anyone doing it.
//
//      make bench-rpc
//
//  THE SHAPE. A custom `protocol_delegate` doing the least framing that is
//  still honest - length prefix, request id, opaque payload - with a handler
//  that echoes a preallocated response and parses nothing. That is deliberate:
//  the number wanted is the TRANSPORT floor, not anyone's future parsing cost.
//
//      [u32 length][u32 request id][payload bytes]
//
//  WHAT IS REPORTED. p50, p99 and p99.9, never a mean. A mean hides exactly the
//  thing that matters: if one lookup in a hundred takes 8 ms, a caller batching
//  a hundred of them is waiting 8 ms, and the average will still read well.
//
//  THE CONTROL. A bare socket ping-pong - two sockets, no reactor, no framing,
//  no library - runs first and is printed alongside. Without it a number like
//  200 us is uninterpretable, because you cannot tell what is this stack and
//  what is the kernel's loopback and scheduler. Read the DIFFERENCE.
//

#include "../TSMoveables/http/server.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if !SNICHOLLS_HAS_HTTP_SERVER

int main()
{
    std::printf("rpc latency: POSIX only - skipped\n");
    return 0;
}

#else

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace snicholls;
using namespace snicholls::http;

namespace {

bool markdown = false;

// ------------------------------------------------------------- the protocol
//
// The whole point of protocol_delegate: our own wire format, their I/O layer.
// consume() is handed whatever arrived and takes what it can parse; anything
// partial stays in the buffer for next time.

constexpr std::size_t kHdr = 8;             // u32 length + u32 request id

class rpc_protocol final : public protocol_delegate {
public:
    explicit rpc_protocol(std::size_t response_bytes)
        : reply_(response_bytes, 'r') {}

    const char* name() const noexcept override { return "bench-rpc"; }
    bool close_after_flush() const noexcept override { return false; }
    bool busy() const noexcept override { return false; }
    bool receiving() const noexcept override { return in_flight_ > 0; }

    bool consume(std::string& in, connection_host& host) override
    {
        // Consumed with an offset, never erase(0, n) per message: erasing the
        // front memmoves everything behind it, so a pipelined batch would cost
        // O(bytes x messages). The same trap the WebSocket client had.
        std::size_t off = 0;
        for (;;) {
            if (in.size() - off < kHdr)
                break;
            const unsigned char* p =
                reinterpret_cast<const unsigned char*>(in.data()) + off;
            const std::uint32_t len = be32(p);
            const std::uint32_t id  = be32(p + 4);
            if (len > (1u << 24))
                return false;                       // absurd frame; drop the peer
            if (in.size() - off < kHdr + len)
                break;                              // partial, wait for the rest
            off += kHdr + len;

            // The reply: header + a preallocated body. No parsing, no
            // allocation of the payload - the transport floor, nothing else.
            char hdr[kHdr];
            put32(hdr, std::uint32_t(reply_.size()));
            put32(hdr + 4, id);                     // correlation echoed back
            host.write_app(hdr, kHdr);
            host.write_app(reply_.data(), reply_.size());
        }
        if (off)
            in.erase(0, off);                       // once per batch, not per message
        return true;
    }

    void respond(std::uint64_t, response&&, connection_host&) override {}

private:
    static std::uint32_t be32(const unsigned char* p)
    {
        return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
               (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
    }
    static void put32(char* p, std::uint32_t v)
    {
        p[0] = char((v >> 24) & 0xff); p[1] = char((v >> 16) & 0xff);
        p[2] = char((v >> 8) & 0xff);  p[3] = char(v & 0xff);
    }

    std::string reply_;
    int in_flight_ = 0;
};

// ------------------------------------------------------------- percentiles

struct dist {
    std::vector<double> us;

    void add(double v) { us.push_back(v); }
    void finish() { std::sort(us.begin(), us.end()); }
    double pct(double p) const
    {
        if (us.empty())
            return 0;
        const std::size_t i =
            std::min(us.size() - 1, std::size_t(p / 100.0 * double(us.size())));
        return us[i];
    }
    std::size_t count() const { return us.size(); }
};

// ------------------------------------------------------------ the control
//
// Two sockets, a fixed-size ping-pong, no reactor and no framing. Whatever this
// costs is the kernel and the scheduler; the stack cannot be faster and should
// not be much slower.

dist loopback_control(std::size_t req, std::size_t resp, int iters)
{
    dist d;
    int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int on = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    if (::bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(lfd, 8) != 0)
        std::exit(1);
    socklen_t alen = sizeof a;
    ::getsockname(lfd, reinterpret_cast<sockaddr*>(&a), &alen);

    std::atomic<bool> ready{false};
    std::thread echo([&] {
        int s = ::accept(lfd, nullptr, nullptr);
        ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
        std::vector<char> buf(req), out(resp, 'r');
        ready.store(true);
        for (int i = 0; i < iters; ++i) {
            std::size_t got = 0;
            while (got < req) {
                const ssize_t n = ::recv(s, buf.data() + got, req - got, 0);
                if (n <= 0) { ::close(s); return; }
                got += std::size_t(n);
            }
            std::size_t sent = 0;
            while (sent < resp) {
                const ssize_t n = ::send(s, out.data() + sent, resp - sent, 0);
                if (n <= 0) { ::close(s); return; }
                sent += std::size_t(n);
            }
        }
        ::close(s);
    });

    int c = ::socket(AF_INET, SOCK_STREAM, 0);
    if (::connect(c, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0)
        std::exit(1);
    ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
    while (!ready.load())
        std::this_thread::yield();

    std::vector<char> out(req, 'q'), in(resp);
    for (int i = 0; i < iters; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        std::size_t sent = 0;
        while (sent < req) {
            const ssize_t n = ::send(c, out.data() + sent, req - sent, 0);
            if (n <= 0) break;
            sent += std::size_t(n);
        }
        std::size_t got = 0;
        while (got < resp) {
            const ssize_t n = ::recv(c, in.data() + got, resp - got, 0);
            if (n <= 0) break;
            got += std::size_t(n);
        }
        d.add(std::chrono::duration<double, std::micro>(
                  std::chrono::steady_clock::now() - t0).count());
    }
    ::close(c);
    echo.join();
    ::close(lfd);
    d.finish();
    return d;
}

// ------------------------------------------------------------- the client
//
// One thread per connection, `depth` requests outstanding at once, each timed
// from its own send to its own reply. Correlation is by request id, so a
// pipelined reply can be matched to the request that earned it.

struct conn_result {
    dist d;
    std::uint64_t done = 0;
};

void put32(char* p, std::uint32_t v)
{
    p[0] = char((v >> 24) & 0xff); p[1] = char((v >> 16) & 0xff);
    p[2] = char((v >> 8) & 0xff);  p[3] = char(v & 0xff);
}
std::uint32_t be32(const unsigned char* p)
{
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}

void run_conn(std::uint16_t port, std::size_t req_bytes, std::size_t depth,
              int per_conn, conn_result& out)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        ::close(fd);
        return;
    }
    const int on = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);

    std::vector<std::chrono::steady_clock::time_point> sent_at(depth + 1);
    std::string body(req_bytes, 'q');
    std::string wire;
    std::string in;
    std::uint32_t next_id = 0;
    std::size_t outstanding = 0;
    int issued = 0;

    auto issue = [&](std::uint32_t id) {
        char hdr[kHdr];
        put32(hdr, std::uint32_t(body.size()));
        put32(hdr + 4, id);
        wire.assign(hdr, kHdr);
        wire += body;
        sent_at[id % (depth + 1)] = std::chrono::steady_clock::now();
        std::size_t off = 0;
        while (off < wire.size()) {
            const ssize_t n = ::send(fd, wire.data() + off, wire.size() - off, 0);
            if (n <= 0)
                return false;
            off += std::size_t(n);
        }
        return true;
    };

    while (outstanding < depth && issued < per_conn) {
        if (!issue(next_id++)) { ::close(fd); return; }
        ++outstanding;
        ++issued;
    }

    std::vector<char> buf(64 * 1024);
    std::size_t in_off = 0;
    while (out.done < std::uint64_t(per_conn)) {
        const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
        if (n <= 0)
            break;
        in.append(buf.data(), std::size_t(n));
        for (;;) {
            if (in.size() - in_off < kHdr)
                break;
            const unsigned char* p =
                reinterpret_cast<const unsigned char*>(in.data()) + in_off;
            const std::uint32_t len = be32(p);
            const std::uint32_t id  = be32(p + 4);
            if (in.size() - in_off < kHdr + len)
                break;
            in_off += kHdr + len;

            out.d.add(std::chrono::duration<double, std::micro>(
                          std::chrono::steady_clock::now() -
                          sent_at[id % (depth + 1)]).count());
            ++out.done;
            --outstanding;
            if (issued < per_conn) {
                if (!issue(next_id++)) break;
                ++outstanding;
                ++issued;
            }
        }
        if (in_off && in_off * 2 >= in.size()) {
            in.erase(0, in_off);
            in_off = 0;
        }
    }
    ::close(fd);
    out.d.finish();
}

struct row {
    const char* label;
    std::size_t conns, depth, req, resp, reactors;
};

// `reactors > 1` starts that many servers on ONE port with SO_REUSEPORT and
// lets the kernel spread accepts across them. The trap worth knowing: plain
// SO_REUSEPORT load-balances on Linux but not on macOS/BSD, where it needs
// SO_REUSEPORT_LB - server.hpp handles that, but it means a macOS result here
// is not evidence for Linux.
dist run_case(const row& r, int total)
{
    const std::size_t n_react = r.reactors ? r.reactors : 1;
    const std::size_t resp = r.resp;

    std::vector<std::unique_ptr<server>> servers;
    std::vector<std::thread> loops;
    // One counter per reactor. protocol_factory runs once per accepted
    // connection, so this measures how the kernel actually spread them -
    // without it, "reuse_port did not help" and "reuse_port did not balance"
    // look identical, and they call for completely different responses.
    std::vector<std::unique_ptr<std::atomic<int>>> accepted;
    for (std::size_t i = 0; i < n_react; ++i)
        accepted.push_back(std::unique_ptr<std::atomic<int>>(new std::atomic<int>(0)));

    std::uint16_t port = 0;
    for (std::size_t i = 0; i < n_react; ++i) {
        server_config cfg;
        cfg.reuse_port = (n_react > 1);
        servers.push_back(std::unique_ptr<server>(new server(cfg)));
        server& srv = *servers.back();
        std::atomic<int>* mine = accepted[i].get();
        srv.protocol_factory([resp, mine](const server_config&) {
            mine->fetch_add(1, std::memory_order_relaxed);
            return std::unique_ptr<protocol_delegate>(new rpc_protocol(resp));
        });
        const std::uint16_t p = srv.listen("127.0.0.1", port);
        if (!p)
            std::exit(1);
        port = p;                       // every later reactor binds the same port
    }
    for (auto& up : servers) {
        server* sp = up.get();
        loops.emplace_back([sp] { sp->run(); });
        while (!sp->running())
            std::this_thread::yield();
    }

    const int per_conn = std::max(1, total / int(r.conns));
    std::vector<conn_result> results(r.conns);
    std::vector<std::thread> ts;
    for (std::size_t i = 0; i < r.conns; ++i)
        ts.emplace_back([&, i] { run_conn(port, r.req, r.depth, per_conn, results[i]); });
    for (auto& t : ts)
        t.join();

    for (auto& up : servers)
        up->stop();
    for (auto& t : loops)
        t.join();

    if (n_react > 1) {
        int total_acc = 0, worst = 0;
        for (auto& c : accepted) {
            const int v = c->load();
            total_acc += v;
            worst = std::max(worst, v);
        }
        std::printf("      accepts per reactor:");
        for (auto& c : accepted)
            std::printf(" %d", c->load());
        std::printf("\n");

        // A benchmark that cannot tell when its own result is meaningless is
        // worse than no benchmark. If the kernel put nearly everything on one
        // reactor then this row measures one reactor plus idle threads, and
        // reporting it as "reuse_port did not help" would be a straight
        // falsehood - it never got the chance to.
        if (total_acc && worst * 100 / total_acc >= 80)
            std::printf("      *** NOT BALANCED - %d%% of accepts on one reactor. This row is\n"
                        "          NOT evidence about reuse_port. Plain SO_REUSEPORT does not\n"
                        "          load-balance on macOS/BSD (needs SO_REUSEPORT_LB); it does on\n"
                        "          Linux. Re-run there before drawing any conclusion.\n",
                        worst * 100 / total_acc);
    }

    dist all;
    for (auto& c : results)
        for (double v : c.d.us)
            all.add(v);
    all.finish();
    return all;
}

void print(const char* label, const dist& d, std::size_t depth)
{
    const double per_lookup = depth ? d.pct(50) / double(depth) : d.pct(50);
    if (markdown)
        std::printf("| %s | %zu | %.0f us | %.0f us | %.0f us | %.0f us |\n",
                    label, d.count(), d.pct(50), d.pct(99), d.pct(99.9), per_lookup);
    else
        std::printf("  %-34s %8zu %9.0f %9.0f %9.0f %11.0f\n",
                    label, d.count(), d.pct(50), d.pct(99), d.pct(99.9), per_lookup);
}

} // namespace

int main(int argc, char** argv)
{
    int total = 20000;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--markdown") == 0) markdown = true;
        else if (std::strcmp(argv[i], "--requests") == 0 && i + 1 < argc)
            total = std::atoi(argv[++i]);
    }

    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    const std::size_t many = std::min<std::size_t>(8, hw);

    const row rows[] = {
        {"1 conn, 1 in flight",          1,  1,   100,  2048, 1},
        {"1 conn, 32 in flight",         1, 32,   100,  2048, 1},
        {"64 conns, 16 in flight",      64, 16,   100,  2048, 1},
        {"64 x 16, N reactors (reuse_port)", 64, 16, 100, 2048, many},
        {"1 conn, 1 in flight, batched", 1,  1,  8192, 65536, 1},
    };

    if (markdown) {
        std::printf("### `make bench-rpc` - request/response latency, loopback\n\n");
        std::printf("| case | samples | p50 | p99 | p99.9 | per in-flight |\n");
        std::printf("|---|---|---|---|---|---|\n");
    } else {
        std::printf("rpc latency - 100 B request, 2 KB response, loopback, one reactor thread\n\n");
        std::printf("  %-34s %8s %9s %9s %9s %11s\n",
                    "case", "samples", "p50", "p99", "p99.9", "per depth");
    }

    // Control first, so every number below is read against it
    const dist ctl = loopback_control(100, 2048, std::min(total, 20000));
    print("bare socket ping-pong (control)", ctl, 1);

    for (const row& r : rows)
        print(r.label, run_case(r, total), r.depth);

    const char* note =
        "\nRead the DIFFERENCE from the control, not the absolute number: the control is\n"
        "two sockets with no reactor and no framing, so whatever it costs is the kernel\n"
        "and the scheduler on this machine. The last column divides p50 by the pipeline\n"
        "depth - the amortised cost per in-flight request, which is the figure that\n"
        "matters to a caller who can batch. p99.9 is reported because a mean hides the\n"
        "case that actually hurts: one request in a thousand stalling long enough to\n"
        "dominate a batch the caller is waiting on.\n"
        "\nLoopback only. A LAN adds its own round trip, which is additive and which you\n"
        "must measure on your own wire - this number is the software floor beneath it.\n";
    std::printf("%s", note);
    return 0;
}

#endif
