//
//  ws_relay_scaling.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - what does an N-in / M-out relay cost, and does the
//  measurement agree with the model?
//
//  The relay is N upstream WebSocket feeds arriving on websocket_clients, each
//  message published into a ws_broadcast_hub, fanned out to M subscribers -
//  all on ONE event loop and one thread, which is the configuration worth
//  measuring because it is the one with no coordination to hide behind.
//
//  THE MODEL. Per inbound message the loop does a fixed amount of work and then
//  a per-subscriber amount:
//
//      cost(N, M) = a + b*M
//
//      a   recv the inbound frame, decode it, build the envelope ONCE
//          (the hub shares one immutable frame, so this is not per subscriber)
//      b   per subscriber: encode the WS frame, copy into that socket's out
//          buffer, and eventually send it
//
//  Two predictions fall out, and both are falsifiable:
//
//    1. Cost is LINEAR in M. If it is not, something is quadratic - a per
//       subscriber copy of something that should have been shared.
//    2. Cost is INDEPENDENT of N at a fixed message count. N is only how many
//       sockets the same messages arrive on; the fan-out work is identical.
//       If cost grows with N, the inbound path is not free the way we think.
//
//  So the sweep is two-dimensional: vary M to fit `a` and `b`, then vary N at
//  fixed M to test prediction 2. The fit is reported next to the measurement
//  rather than instead of it, and the residual is printed - a model that is
//  only shown where it agrees is decoration.
//
//      make bench-relay
//
//  What this does NOT measure, stated because the mistake is easy and this
//  benchmark's sibling made it: the number here is the RELAY's cost, taken
//  from the hub's own delivered counter on the server side. Subscribers are
//  drained by poll() threads in this same process, and if those threads
//  saturate they cap the measurement rather than the relay doing so. `--readers`
//  exists so that can be checked instead of assumed - if the numbers move when
//  you change it, you are measuring the harness.
//

#include "../TSMoveables/http/websocket_client.hpp"
#include "../TSMoveables/http/ws_broadcast_hub.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if !SNICHOLLS_HAS_WEBSOCKET_CLIENT

int main()
{
    std::printf("ws relay bench: POSIX only - skipped\n");
    return 0;
}

#else

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
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
std::size_t reader_threads = 4;

double seconds_since(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// ------------------------------------------------------- subscriber plumbing

int connect_subscriber(std::uint16_t port, const char* path)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { ::close(fd); return -1; }
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
    std::string head;
    while (head.find("\r\n\r\n") == std::string::npos) {
        char t[512];
        const ssize_t n = ::recv(fd, t, sizeof t, 0);
        if (n <= 0) { ::close(fd); return -1; }
        head.append(t, std::size_t(n));
    }
    if (head.compare(9, 3, "101") != 0) { ::close(fd); return -1; }
    ::fcntl(fd, F_SETFL, O_NONBLOCK);
    return fd;
}

// Drain and discard. The relay's own counter is the measurement; these threads
// exist only so backpressure never gates it.
struct drain {
    std::vector<int> fds;
    std::atomic<bool> stop{false};

    void run()
    {
        std::vector<pollfd> pf(fds.size());
        for (std::size_t i = 0; i < fds.size(); ++i) { pf[i].fd = fds[i]; pf[i].events = POLLIN; }
        std::vector<char> buf(256 * 1024);
        while (!stop.load(std::memory_order_relaxed)) {
            const int k = ::poll(pf.data(), nfds_t(pf.size()), 50);
            if (k <= 0)
                continue;
            for (auto& p : pf) {
                if (!(p.revents & POLLIN))
                    continue;
                for (;;) {
                    const ssize_t n = ::recv(p.fd, buf.data(), buf.size(), 0);
                    if (n <= 0 || std::size_t(n) < buf.size())
                        break;
                }
            }
        }
    }
};

struct cell {
    std::size_t n = 0, m = 0;
    double per_msg_us = 0;      // relay cost per INBOUND message
    double deliveries = 0;      // frames/s the relay handed to sockets
    bool complete = false;      // did every expected frame arrive
};

// ------------------------------------------------------------- the relay run

cell run_relay(std::size_t n_in, std::size_t m_out, std::uint64_t msgs, std::size_t payload,
               bool inbound_own_threads = false)
{
    // --- upstream: one server holding N live sockets we can push down
    server up;
    std::vector<websocket> feeds;
    moveable_mutex<> feeds_mtx;
    up.get("/feed", websocket_route([&](websocket ws) {
        std::lock_guard<moveable_mutex<>> g(feeds_mtx);
        feeds.push_back(ws);
    }));
    const std::uint16_t up_port = up.listen("127.0.0.1", 0);
    if (!up_port) std::exit(1);
    std::thread up_thread([&up] { up.run(); });
    while (!up.running()) std::this_thread::yield();

    // --- relay: the hub's server AND the N inbound clients on ONE loop
    ws_hub_config hcfg;
    hcfg.replay_on_connect = false;
    hcfg.max_subscribers = m_out + 16;
    hcfg.max_queue_msgs = 1u << 16;
    hcfg.max_queue_bytes = 512u * 1024 * 1024;
    hcfg.send_high_water = 64u * 1024 * 1024;   // let the drain threads, not backpressure, set the pace

    server relay;
    ws_broadcast_hub hub{hcfg};
    hub.mount(relay);
    const std::uint16_t relay_port = relay.listen("127.0.0.1", 0);
    if (!relay_port) std::exit(1);

    ws_client_config ccfg;
    ccfg.auto_reconnect = false;                // a failure should show up, not be papered over
    std::vector<std::unique_ptr<websocket_client>> ins;
    for (std::size_t i = 0; i < n_in; ++i) {
        auto c = std::unique_ptr<websocket_client>(new websocket_client(ccfg));
        c->on_message([&hub](const ws_message& m) { hub.publish("relay", m.data); });
        ins.push_back(std::move(c));
    }
    std::thread relay_thread([&relay] { relay.run(); });
    while (!relay.running()) std::this_thread::yield();

    // Either every inbound client shares the relay's loop (one thread does
    // everything), or each gets a loop and a thread of its own. Same work, same
    // hub; the only difference is how many cores the inbound side may use.
    std::vector<std::unique_ptr<event_loop>> in_loops;
    std::vector<std::thread> in_threads;
    const std::string url = "ws://127.0.0.1:" + std::to_string(up_port) + "/feed";
    if (inbound_own_threads) {
        for (std::size_t i = 0; i < n_in; ++i) {
            in_loops.push_back(std::unique_ptr<event_loop>(new event_loop()));
            ins[i]->connect(*in_loops.back(), url);
        }
        for (auto& l : in_loops) {
            event_loop* lp = l.get();
            in_threads.emplace_back([lp] { lp->run(); });
            while (!lp->running()) std::this_thread::yield();
        }
    } else {
        for (auto& c : ins)
            c->connect(relay.loop(), url);
    }

    // every client attached upstream, and upstream saw all of them
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (;;) {
        std::size_t ready = 0;
        for (auto& c : ins) ready += c->connected() ? 1 : 0;
        std::size_t seen = 0;
        { std::lock_guard<moveable_mutex<>> g(feeds_mtx); seen = feeds.size(); }
        if ((ready == n_in && seen == n_in) || std::chrono::steady_clock::now() > deadline)
            break;
        std::this_thread::yield();
    }

    // --- M subscribers on the relay
    std::vector<int> subs;
    subs.reserve(m_out);
    for (std::size_t i = 0; i < m_out; ++i) {
        const int fd = connect_subscriber(relay_port, "/ws?topic=relay");
        if (fd < 0) break;
        subs.push_back(fd);
    }
    const std::size_t m = subs.size();
    while (hub.subscribers() < m) std::this_thread::yield();

    const std::size_t dthreads = std::min<std::size_t>(m, reader_threads);
    std::vector<drain> drains(dthreads ? dthreads : 1);
    for (std::size_t i = 0; i < m; ++i) drains[i % drains.size()].fds.push_back(subs[i]);
    std::vector<std::thread> dts;
    for (auto& d : drains)
        if (!d.fds.empty()) dts.emplace_back([&d] { d.run(); });

    // --- drive: msgs messages spread evenly over the N upstream sockets
    const std::string payload_str(payload, 'x');
    const std::uint64_t want = msgs * m;
    const std::uint64_t base = hub.snapshot().delivered;

    const auto t0 = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < msgs; ++i) {
        std::lock_guard<moveable_mutex<>> g(feeds_mtx);
        feeds[i % feeds.size()].send_text(payload_str);
    }

    const auto hard = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    for (;;) {
        if (hub.snapshot().delivered - base >= want || std::chrono::steady_clock::now() > hard)
            break;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    const double secs = seconds_since(t0);
    const std::uint64_t got = hub.snapshot().delivered - base;

    for (auto& d : drains) d.stop.store(true);
    for (auto& t : dts) t.join();
    for (int fd : subs) ::close(fd);
    for (auto& c : ins) c->close();
    for (auto& l : in_loops) l->stop();
    for (auto& t : in_threads) t.join();
    relay.stop(); relay_thread.join();
    up.stop();    up_thread.join();

    cell r;
    r.n = n_in;
    r.m = m;
    r.complete = (got >= want);
    r.per_msg_us = secs * 1e6 / double(msgs ? msgs : 1);
    r.deliveries = double(got) / (secs > 0 ? secs : 1);
    return r;
}

// Least squares through (M, cost) - the model is cost = a + b*M.
//
// Fitted only over the points where the RELAY is the bottleneck. At small M it
// is not: the loop finishes each message long before the next arrives, so the
// measurement is the feeder's rate, not the relay's capacity, and including
// those points drags the intercept negative - which is how you can tell they do
// not belong to the same line rather than a reason to quietly drop them.
void fit(const std::vector<cell>& cs, double& a, double& b, std::size_t min_m)
{
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    double k = 0;
    for (const auto& c : cs) {
        if (c.m < min_m) continue;
        ++k;
        const double x = double(c.m), y = c.per_msg_us;
        sx += x; sy += y; sxx += x * x; sxy += x * y;
    }
    if (k < 2) { a = 0; b = 0; return; }
    const double den = k * sxx - sx * sx;
    b = den != 0 ? (k * sxy - sx * sy) / den : 0;
    a = (sy - b * sx) / k;
}

} // namespace

int main(int argc, char** argv)
{
    std::uint64_t msgs = 2000;
    std::size_t payload = 256;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--markdown") == 0) markdown = true;
        else if (std::strcmp(argv[i], "--msgs") == 0 && i + 1 < argc) msgs = std::strtoull(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--payload") == 0 && i + 1 < argc) payload = std::size_t(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--readers") == 0 && i + 1 < argc) reader_threads = std::size_t(std::atoi(argv[++i]));
    }

    const std::size_t m_sweep[] = {1, 10, 50, 100, 200};
    const std::size_t n_sweep[] = {1, 2, 4, 8};

    std::vector<cell> by_m;
    for (std::size_t m : m_sweep)
        by_m.push_back(run_relay(4, m, msgs, payload));

    double a = 0, b = 0;
    static const std::size_t kSaturated = 50;
    fit(by_m, a, b, kSaturated);

    if (markdown) {
        std::printf("### `make bench-relay` - N-in / M-out, measured against `a + b*M`\n\n");
        std::printf("Fitted on one loop thread: **a = %.1f us** fixed per inbound message, "
                    "**b = %.2f us** per subscriber.\n\n", a, b);
        std::printf("| N in | M out | measured | model `a+b*M` | residual | deliveries/s |\n");
        std::printf("|---|---|---|---|---|---|\n");
    } else {
        std::printf("relay scaling - %llu messages, %zu-byte payload, one loop thread\n\n",
                    (unsigned long long)msgs, payload);
        std::printf("  model: cost per inbound message = a + b*M\n");
        std::printf("  fitted a = %.1f us (fixed)   b = %.2f us (per subscriber)\n\n", a, b);
        std::printf("  %-5s %-6s %12s %14s %10s %14s\n",
                    "N in", "M out", "measured", "model", "residual", "deliveries/s");
    }

    auto row = [&](const cell& c) {
        const double model = a + b * double(c.m);
        const double resid = c.per_msg_us - model;
        const char* flag = c.complete ? "" : "  (INCOMPLETE)";
        if (markdown)
            std::printf("| %zu | %zu | %.1f us | %.1f us | %+.1f us | %.0f%s |\n",
                        c.n, c.m, c.per_msg_us, model, resid, c.deliveries, flag);
        else
            std::printf("  %-5zu %-6zu %9.1f us %11.1f us %8.1f%% %14.0f%s\n",
                        c.n, c.m, c.per_msg_us, model,
                        model != 0 ? 100.0 * resid / model : 0.0, c.deliveries, flag);
    };

    for (const auto& c : by_m) row(c);

    // Prediction 2: at fixed M, cost should not care how many sockets the same
    // messages arrived on.
    if (markdown)
        std::printf("\n**N-independence** (fixed M = 50): the fan-out work is identical, so cost "
                    "should be flat across N. Any slope here is inbound-path cost.\n\n"
                    "| N in | M out | measured | deliveries/s |\n|---|---|---|---|\n");
    else
        std::printf("\n  N-independence at fixed M = 50 (cost should be flat):\n");

    std::vector<cell> by_n;
    for (std::size_t n : n_sweep) {
        const cell c = run_relay(n, 50, msgs, payload);
        by_n.push_back(c);
        if (markdown)
            std::printf("| %zu | %zu | %.1f us | %.0f |\n", c.n, c.m, c.per_msg_us, c.deliveries);
        else
            std::printf("  %-5zu %-6zu %9.1f us %14.0f\n", c.n, c.m, c.per_msg_us, c.deliveries);
    }

    // The threading question, measured rather than argued: same N, same M, same
    // messages - inbound sharing the relay loop, then inbound on N loops of
    // their own. If "N inbound sockets wants N threads" were right, the second
    // row would be the faster one.
    if (markdown)
        std::printf("\n**Does the inbound side want its own threads?** Same work, N=8, M=50.\n\n"
                    "| inbound placement | threads | measured | deliveries/s |\n|---|---|---|---|\n");
    else
        std::printf("\n  inbound threading A/B (N=8, M=50):\n");
    {
        const cell shared = run_relay(8, 50, msgs, payload, false);
        const cell owned  = run_relay(8, 50, msgs, payload, true);
        if (markdown) {
            std::printf("| shares the relay loop | 1 | %.1f us | %.0f |\n",
                        shared.per_msg_us, shared.deliveries);
            std::printf("| one loop per inbound socket | 8 | %.1f us | %.0f |\n",
                        owned.per_msg_us, owned.deliveries);
        } else {
            std::printf("  %-30s %2d thread   %9.1f us %12.0f\n",
                        "shares the relay loop", 1, shared.per_msg_us, shared.deliveries);
            std::printf("  %-30s %2d threads  %9.1f us %12.0f\n",
                        "one loop per inbound socket", 8, owned.per_msg_us, owned.deliveries);
        }
        const double gain = shared.per_msg_us > 0
            ? 100.0 * (shared.per_msg_us - owned.per_msg_us) / shared.per_msg_us : 0.0;
        std::printf("%s  eight extra threads bought %+.0f%%\n", markdown ? "\n" : "", gain);
    }

    double lo = 1e30, hi = 0;
    for (const auto& c : by_n) { lo = std::min(lo, c.per_msg_us); hi = std::max(hi, c.per_msg_us); }
    const double spread = lo > 0 ? 100.0 * (hi - lo) / lo : 0.0;

    const char* note =
        "\nThe fit is reported next to the measurement, not instead of it. Linearity in M is the\n"
        "claim that the envelope is built once and only the per-socket encode and copy repeat;\n"
        "flatness in N is the claim that the inbound path costs nothing the fan-out does not.\n"
        "Real sockets over loopback in one process, so the kernel and this harness's drain\n"
        "threads are in every number - vary --readers, and if the results move, that is what\n"
        "you are measuring.\n";
    if (markdown)
        std::printf("\nSpread across N: **%.0f%%**.\n%s", spread, note);
    else
        std::printf("\n  spread across N: %.0f%%\n%s", spread, note);
    return 0;
}

#endif
