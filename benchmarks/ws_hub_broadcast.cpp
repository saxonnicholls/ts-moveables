//
//  ws_hub_broadcast.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - what does fanning one message out to N sockets cost?
//
//  The hub's pitch is fan-out: one publish, many subscribers, and a publisher
//  that does not pay for the crowd. So the number this leads with is **what
//  publish() costs the thread that calls it**, swept across subscriber counts.
//  A webhook handler calling publish() is the customer here, and what it wants
//  to know is whether its cost grows with an audience it never sees.
//
//      make bench-wshub
//
//  A note on what this file does NOT measure, written down because getting it
//  wrong is the easy mistake. The first version of this benchmark led with
//  end-to-end delivered frames/s and read a 4.5x drop between 10 and 100
//  subscribers as a fan-out scaling problem. It was not: raising --readers
//  from 4 to 32 moved that same number from 98,000 to 4,500 frames/s, which
//  is this benchmark's own poll() loop saturating, not the hub. Delivered
//  frames/s over loopback measures the client and the kernel at least as much
//  as the server, so it is still reported - it is the only end-to-end figure
//  here - but it is reported last and it is not a claim about the hub.
//
//  `--readers N` is kept precisely so that caveat stays checkable rather than
//  being something you have to take on trust.
//

#include "../TSMoveables/http/ws_broadcast_hub.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if !SNICHOLLS_HAS_WS_BROADCAST_HUB

int main()
{
    std::printf("ws hub bench: POSIX only - skipped\n");
    return 0;
}

#else

#include <algorithm>
#include <atomic>
#include <chrono>
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
std::size_t reader_threads = 8;   // --readers, to test whether the client is the limit

double seconds_since(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// ------------------------------------------------------------- a subscriber
//
// Just enough WebSocket to connect and count frames. Server-to-client frames
// are never masked, so consuming one is: opcode byte, length byte (with the
// two extended forms), then that many payload bytes.

int connect_subscriber(std::uint16_t port, const char* path)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        ::close(fd);
        return -1;
    }
    const int on = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);

    const std::string req =
        std::string("GET ") + path + " HTTP/1.1\r\nHost: b\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    std::size_t off = 0;
    while (off < req.size()) {
        const ssize_t n = ::send(fd, req.data() + off, req.size() - off, 0);
        if (n <= 0) { ::close(fd); return -1; }
        off += std::size_t(n);
    }

    // Drain exactly the handshake response, leaving the socket at a frame
    // boundary. The hub sends nothing before the 101 completes.
    std::string head;
    while (head.find("\r\n\r\n") == std::string::npos) {
        char t[512];
        const ssize_t n = ::recv(fd, t, sizeof t, 0);
        if (n <= 0) { ::close(fd); return -1; }
        head.append(t, std::size_t(n));
    }
    if (head.compare(9, 3, "101") != 0) { ::close(fd); return -1; }

    ::fcntl(fd, F_SETFL, O_NONBLOCK);       // the reader polls; recv must not block
    return fd;
}

// Count whole frames sitting in `buf`, erasing what it consumes. Partial
// frames stay for the next read.
std::size_t consume_frames(std::string& buf, std::uint64_t& bytes)
{
    std::size_t frames = 0, off = 0;
    for (;;) {
        if (buf.size() - off < 2)
            break;
        const unsigned char b1 = static_cast<unsigned char>(buf[off + 1]);
        std::uint64_t len = b1 & 0x7f;
        std::size_t hdr = 2;
        if (len == 126) {
            if (buf.size() - off < 4) break;
            len = (std::uint64_t(static_cast<unsigned char>(buf[off + 2])) << 8) |
                  std::uint64_t(static_cast<unsigned char>(buf[off + 3]));
            hdr = 4;
        } else if (len == 127) {
            if (buf.size() - off < 10) break;
            len = 0;
            for (int i = 0; i < 8; ++i)
                len = (len << 8) | std::uint64_t(static_cast<unsigned char>(buf[off + 2 + i]));
            hdr = 10;
        }
        if (buf.size() - off < hdr + len)
            break;
        off += hdr + std::size_t(len);
        bytes += len;
        ++frames;
    }
    if (off)
        buf.erase(0, off);
    return frames;
}

// One thread drains a slice of the subscribers with poll(), so a quiet socket
// never holds up a busy one - the same reason the server itself is a reactor.
struct reader {
    std::vector<int> fds;
    std::atomic<std::uint64_t> frames{0};
    std::atomic<std::uint64_t> bytes{0};

    void run(std::uint64_t want, std::atomic<bool>& stop)
    {
        std::vector<pollfd> pf(fds.size());
        std::vector<std::string> buf(fds.size());
        for (std::size_t i = 0; i < fds.size(); ++i) {
            pf[i].fd = fds[i];
            pf[i].events = POLLIN;
        }
        std::uint64_t got = 0, by = 0;
        while (got < want && !stop.load(std::memory_order_relaxed)) {
            const int k = ::poll(pf.data(), nfds_t(pf.size()), 250);
            if (k <= 0)
                continue;
            for (std::size_t i = 0; i < pf.size(); ++i) {
                if (!(pf[i].revents & POLLIN))
                    continue;
                char t[64 * 1024];
                for (;;) {
                    const ssize_t n = ::recv(pf[i].fd, t, sizeof t, 0);
                    if (n <= 0)
                        break;
                    buf[i].append(t, std::size_t(n));
                    if (std::size_t(n) < sizeof t)
                        break;
                }
                got += consume_frames(buf[i], by);
            }
            frames.store(got, std::memory_order_relaxed);
            bytes.store(by, std::memory_order_relaxed);
        }
        frames.store(got, std::memory_order_relaxed);
        bytes.store(by, std::memory_order_relaxed);
    }
};

struct result {
    std::size_t subscribers = 0;
    std::uint64_t msgs = 0;             // publish() calls
    std::uint64_t frames = 0;           // frames the subscribers actually read
    double pub_secs = 0;                // time inside the publish loop
    double secs = 0;                    // publish + drain, end to end
    double mb = 0;
};

result run_case(std::size_t subs, std::uint64_t total_frames, ws_hub_framing mode,
                std::size_t payload_bytes)
{
    ws_hub_config cfg;
    cfg.frame_mode = mode;
    cfg.replay_on_connect = false;          // measure live fan-out, not history
    cfg.max_subscribers = subs + 16;
    cfg.max_queue_msgs = 1u << 16;          // a bounded queue that will not trim
    cfg.max_queue_bytes = 256u * 1024 * 1024;

    server srv;
    ws_broadcast_hub hub{cfg};
    hub.mount(srv);
    const std::uint16_t port = srv.listen("127.0.0.1", 0);
    if (!port)
        std::exit(1);
    std::thread loop([&srv] { srv.run(); });
    while (!srv.running())
        std::this_thread::yield();

    std::vector<int> fds;
    fds.reserve(subs);
    for (std::size_t i = 0; i < subs; ++i) {
        const int fd = connect_subscriber(port, "/ws?topic=bench");
        if (fd < 0) {
            std::fprintf(stderr, "ws hub bench: only %zu of %zu subscribers connected - "
                                 "raise the descriptor limit (ulimit -n)\n", fds.size(), subs);
            break;
        }
        fds.push_back(fd);
    }
    const std::size_t n = fds.size();
    while (hub.subscribers() < n)
        std::this_thread::yield();

    const std::uint64_t msgs = std::max<std::uint64_t>(1, total_frames / std::max<std::size_t>(n, 1));
    const std::uint64_t want = msgs * n;

    const std::size_t threads = std::min<std::size_t>(n, reader_threads);
    std::vector<reader> readers(threads);
    for (std::size_t i = 0; i < n; ++i)
        readers[i % threads].fds.push_back(fds[i]);

    std::atomic<bool> stop{false};
    std::vector<std::thread> rt;
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < threads; ++i) {
        const std::uint64_t share = msgs * readers[i].fds.size();
        rt.emplace_back([&readers, i, share, &stop] { readers[i].run(share, stop); });
    }

    const std::string payload(payload_bytes, 'x');
    const auto p0 = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < msgs; ++i)
        hub.publish("bench", payload);
    const double pub_secs = seconds_since(p0);

    // A generous ceiling: this is a backstop against a hang, not a timing
    // assertion. Whatever arrived by then is what gets reported.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (;;) {
        std::uint64_t got = 0;
        for (auto& r : readers)
            got += r.frames.load(std::memory_order_relaxed);
        if (got >= want || std::chrono::steady_clock::now() > deadline)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const double secs = seconds_since(t0);
    stop.store(true);
    for (auto& t : rt)
        t.join();

    result out;
    out.subscribers = n;
    out.msgs = msgs;
    out.pub_secs = pub_secs;
    out.secs = secs;
    for (auto& r : readers) {
        out.frames += r.frames.load();
        out.mb += double(r.bytes.load());
    }
    out.mb /= (1024.0 * 1024.0);

    for (int fd : fds)
        ::close(fd);
    srv.stop();
    loop.join();
    return out;
}

void row(const result& r, const char* label)
{
    // The publish side is the hub's own cost - what the thread calling
    // publish() pays. The delivered rate is the whole system including this
    // benchmark's own reader, which is why it is reported but not led with.
    const double pub_ns   = r.pub_secs * 1e9 / double(r.msgs ? r.msgs : 1);
    const double fanout_ns = r.pub_secs * 1e9 /
                             double(r.msgs * (r.subscribers ? r.subscribers : 1));
    const double fps      = r.frames / (r.secs > 0 ? r.secs : 1);

    if (markdown)
        std::printf("| %s | %zu | %.0f ns | %.0f ns | %.0f |\n",
                    label, r.subscribers, pub_ns, fanout_ns, fps);
    else
        std::printf("  %-9s %5zu subs   publish %8.0f ns   per subscriber %6.0f ns"
                    "   delivered %9.0f frames/s\n",
                    label, r.subscribers, pub_ns, fanout_ns, fps);
}

} // namespace

int main(int argc, char** argv)
{
    std::uint64_t total = 200000;
    std::size_t payload = 64;
    std::size_t only_subs = 0;   // --subs N: measure one subscriber count instead of the sweep
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--markdown") == 0) markdown = true;
        else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) total = std::strtoull(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--payload") == 0 && i + 1 < argc) payload = std::size_t(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--readers") == 0 && i + 1 < argc) reader_threads = std::size_t(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--subs") == 0 && i + 1 < argc) only_subs = std::size_t(std::atoi(argv[++i]));
    }

    std::vector<std::size_t> sweep = {1, 10, 100, 500};
    if (only_subs) sweep = {only_subs};

    if (markdown) {
        std::printf("### `make bench-wshub` - what publishing costs the caller\n\n");
        std::printf("| Framing | Subscribers | Per publish | Per subscriber | Delivered |\n");
        std::printf("|---|---|---|---|---|\n");
    } else {
        std::printf("ws_broadcast_hub - what publish() costs the calling thread\n"
                    "~%llu frames per case, %zu-byte payload\n\n",
                    (unsigned long long)total, payload);
    }

    for (std::size_t s : sweep)
        row(run_case(s, total, ws_hub_framing::envelope_json, payload), "envelope");
    for (std::size_t s : sweep)
        row(run_case(s, total, ws_hub_framing::raw, payload), "raw");

    if (markdown)
        std::printf("\nReal sockets over loopback, so the rate includes the kernel's send path "
                    "and this machine's scheduler. Read the shape across the sweep - fan-out "
                    "should hold its league as subscribers grow - and the envelope-vs-raw gap, "
                    "which is what the JSON envelope costs.\n");
    else
        std::printf("\n  Real sockets over loopback: a good part of this is the kernel, not the\n"
                    "  hub. The shape across the sweep is the claim - fan-out holding its\n"
                    "  league as subscribers grow - and raw-vs-envelope is the JSON cost.\n");
    return 0;
}

#endif
