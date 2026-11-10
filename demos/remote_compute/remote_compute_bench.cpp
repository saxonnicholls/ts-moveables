//
//  remote_compute_bench.cpp
//  TSMoveables - demos/remote_compute (from the remote_compute_example prototype at 92d8dc4)
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Where does the time in a remote call go? Each row adds one layer:
//
//    1. native            add(a, b) through a function pointer
//    2. codec + dispatch  encode args, perfect-hash lookup, invoke, encode and
//                         decode the result - all in this process, no threads
//    3. thread hop        hand add() to a pool thread, wait on a std::future
//    4. raw TCP           1-byte ping-pong over a socket, no protocol at all:
//                         the network and the kernel's TCP stack alone
//    5. remote call       the full stack: c.async(on{...}, add, a, b).get()
//
//  Then payload sizes (echo of 8 B to 4 MB) for bandwidth, and pipelined calls
//  for throughput. Latencies are medians, with p99, over many calls.
//
//    bench_remote                      everything on loopback, workers in-process
//    bench_remote echo-server <port>   a raw TCP echo for row 4 (run on the far end)
//    bench_remote worker <name> <port> a worker for row 5 (run on the far end)
//    bench_remote remote <name> <host> <worker-port> <echo-port>
//

#include "kernels.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <string>
#include <thread>
#include <vector>

using namespace example;
namespace r = snicholls::remote;
using clk = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {

struct stats {
    double median_ns, p99_ns;
};

template <typename F>
stats time_each(int n, F&& f)
{
    std::vector<double> ns;
    ns.reserve(std::size_t(n));
    for (int i = 0; i < n / 10; ++i)
        f();   // warm up
    for (int i = 0; i < n; ++i) {
        const auto t0 = clk::now();
        f();
        ns.push_back(std::chrono::duration<double, std::nano>(clk::now() - t0).count());
    }
    std::sort(ns.begin(), ns.end());
    return {ns[ns.size() / 2], ns[std::min(ns.size() - 1, ns.size() * 99 / 100)]};
}

// Rows 1 and 2 are too fast to time one call at a time: time a batch
template <typename F>
double time_batch_ns(long n, F&& f)
{
    for (long i = 0; i < n / 10; ++i)
        f(i);
    const auto t0 = clk::now();
    for (long i = 0; i < n; ++i)
        f(i);
    return std::chrono::duration<double, std::nano>(clk::now() - t0).count() / double(n);
}

std::string human_ns(double ns)
{
    char b[32];
    if (ns < 1000)
        std::snprintf(b, sizeof b, "%.1f ns", ns);
    else if (ns < 1e6)
        std::snprintf(b, sizeof b, "%.1f us", ns / 1e3);
    else
        std::snprintf(b, sizeof b, "%.2f ms", ns / 1e6);
    return b;
}

void row(const char* name, double median, double p99, const char* note)
{
    std::printf("  %-22s %12s %12s   %s\n", name, human_ns(median).c_str(), p99 > 0 ? human_ns(p99).c_str() : "",
                note);
}

volatile std::int64_t sink;
std::int64_t (*volatile add_ptr)(std::int64_t, std::int64_t) = &add;

// --------------------------------------------------------------- raw echo

void serve_echo(snicholls::net::tcp_listener& l, std::atomic<bool>& stop)
{
    while (!stop) {
        auto s = l.accept(200ms);
        if (!s.valid())
            continue;
        s.set_nodelay(true);
        std::thread([s = std::move(s), &stop]() mutable {
            char buf[64 * 1024];
            while (!stop) {
                const auto n = s.read(buf, sizeof buf, 250ms);
                if (n == 0 || (n < 0 && errno != ETIMEDOUT))
                    break;
                if (n > 0 && !s.write_all(buf, std::size_t(n), 5s))
                    break;
            }
        }).detach();
    }
}

stats raw_ping(const std::string& host, std::uint16_t port, int n)
{
    auto s = snicholls::net::tcp_stream::connect(host, port, 3s);
    if (!s.valid())
        throw std::runtime_error("cannot reach the echo server");
    s.set_nodelay(true);
    char c = 'x';
    return time_each(n, [&] {
        s.write_all(&c, 1, 5s);
        s.read(&c, 1, 5s);
    });
}

// ------------------------------------------------------------------ report

void layers(r::cluster<kernels>& c, const std::string& node, const std::string& echo_host, std::uint16_t echo_port,
            const char* where)
{
    std::printf("\nlatency of one call, %s (median, p99)\n", where);
    std::printf("  %-22s %12s %12s\n", "layer", "median", "p99");

    const double native = time_batch_ns(20'000'000, [](long i) { sink = add_ptr(i, 1); });
    row("1 native call", native, 0, "add(a, b) through a function pointer");

    constexpr int k = kernels::find("add");
    const double codec = time_batch_ns(2'000'000, [](long i) {
        std::string args, out;
        r::encode(args, std::int64_t(i), std::int64_t(1));
        const int idx = kernels::find("add");
        kernels::invokers[idx](args, out);
        sink = r::reader(out).get<std::int64_t>();
    });
    static_assert(k >= 0);
    row("2 + codec, dispatch", codec, 0, "encode, perfect-hash lookup, invoke, decode; one thread");

    snicholls::work_stealing_task_pool pool(2);
    const auto hop = time_each(20000, [&] {
        std::promise<std::int64_t> p;
        auto f = p.get_future();
        pool.submit([&p] { p.set_value(add(2, 3)); });
        sink = f.get();
    });
    row("3 + thread hop", hop.median_ns, hop.p99_ns, "to a pool thread and back through a std::future");

    const int n = 20000;
    const auto ping = raw_ping(echo_host, echo_port, n);
    row("4 raw TCP round trip", ping.median_ns, ping.p99_ns, "1-byte ping-pong, no protocol");

    const auto full = time_each(n, [&] { sink = c.async(r::on{node}, add, 2, 3).get(); });
    row("5 remote call", full.median_ns, full.p99_ns, "c.async(on{...}, add, 2, 3).get()");

    const auto none = time_each(n, [&] { c.async(r::on{node}, noop).get(); });
    row("  remote noop", none.median_ns, none.p99_ns, "no arguments, no result");

    std::printf("\n  where the remote call's time goes (medians):\n");
    const double proto = full.median_ns - ping.median_ns;
    std::printf("    network round trip (row 4)          %10s  %4.0f%%\n", human_ns(ping.median_ns).c_str(),
                100 * ping.median_ns / full.median_ns);
    std::printf("    protocol, codec, threads (5 - 4)    %10s  %4.0f%%\n", human_ns(proto).c_str(),
                100 * proto / full.median_ns);
    std::printf("      of which codec + dispatch          %10s\n", human_ns(codec - native).c_str());
    std::printf("      of which thread hand-offs         ~%10s  (two: worker pool, client reader)\n",
                human_ns(2 * hop.median_ns).c_str());
    std::printf("    the work itself (row 1)             %10s\n", human_ns(native).c_str());
    std::printf("  break-even: remote pays off when a call's work exceeds ~%s on this link\n",
                human_ns(full.median_ns).c_str());

    std::printf("\npayload size, echo there and back, %s\n", where);
    std::printf("  %-12s %12s %14s\n", "bytes each way", "median", "throughput");
    for (std::size_t size : {8u, 1024u, 65536u, 1048576u, 4194304u}) {
        const std::string payload(size, 'x');
        const int reps = size >= 1048576 ? 30 : 500;
        const auto t = time_each(reps, [&] {
            const auto back = c.async(r::on{node}, echo, payload).get();
            if (back.size() != size)
                throw std::runtime_error("echo came back the wrong size");
        });
        const double mbps = 2.0 * double(size) / (t.median_ns / 1e9) / 1e6;
        std::printf("  %-12zu %12s %11.1f MB/s\n", size, human_ns(t.median_ns).c_str(), mbps);
    }

    std::printf("\npipelined: many calls in flight, %s\n", where);
    for (int in_flight : {1, 16, 256}) {
        const int total = 20000;
        const auto t0 = clk::now();
        std::vector<std::future<std::int64_t>> fs;
        fs.reserve(std::size_t(in_flight));
        for (int done = 0; done < total;) {
            for (int i = 0; i < in_flight && done + i < total; ++i)
                fs.push_back(c.async(r::on{node}, add, i, 1));
            done += int(fs.size());
            for (auto& f : fs)
                sink = f.get();
            fs.clear();
        }
        const double per = std::chrono::duration<double, std::nano>(clk::now() - t0).count() / total;
        std::printf("  %4d in flight: %10s per call, %9.0f calls/s\n", in_flight, human_ns(per).c_str(), 1e9 / per);
    }
}

} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        if (argc >= 3 && std::strcmp(argv[1], "echo-server") == 0) {
            auto l = snicholls::net::tcp_listener::bind("0.0.0.0", std::uint16_t(std::stoi(argv[2])));
            std::atomic<bool> stop{false};
            std::printf("raw echo on port %u\n", unsigned(l.port()));
            serve_echo(l, stop);
            return 0;
        }
        if (argc >= 4 && std::strcmp(argv[1], "worker") == 0) {
            r::worker<kernels> w(r::load_key(), argv[2], "0.0.0.0", std::uint16_t(std::stoi(argv[3])));
            std::printf("worker '%s' on port %u\n", argv[2], unsigned(w.port()));
            for (;;)
                std::this_thread::sleep_for(1s);
        }
        if (argc >= 6 && std::strcmp(argv[1], "remote") == 0) {
            r::cluster<kernels> c(r::load_key());
            c.add(argv[2], argv[3], std::uint16_t(std::stoi(argv[4])));
            const auto info = c.nodes().front();
            const std::string where = "to " + info.name + " (" + info.host + ", " + info.arch + ") over the network";
            layers(c, argv[2], argv[3], std::uint16_t(std::stoi(argv[5])), where.c_str());
            return 0;
        }

        // Everything on loopback, in this process
        const std::string key = r::random_bytes(32);
        r::worker<kernels> w(key, "local", "127.0.0.1", 0, 2);
        auto echo_l = snicholls::net::tcp_listener::bind("127.0.0.1", 0);
        std::atomic<bool> stop{false};
        std::thread echo_t([&] { serve_echo(echo_l, stop); });
        r::cluster<kernels> c(key);
        c.add("local", "127.0.0.1", w.port());
        layers(c, "local", "127.0.0.1", echo_l.port(), "on loopback (this machine)");
        stop = true;
        echo_l.shutdown();
        echo_t.join();
        std::this_thread::sleep_for(300ms);   // let detached echo threads see stop
        return 0;
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
