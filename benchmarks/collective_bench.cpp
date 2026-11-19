//
//  collective_bench.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - allreduce_sum, broadcast and all_to_all_v throughput
//  on loopback (net/collective.hpp), each rank a process, 0.5 GB of floats
//  (the all-to-all: each rank's buffer cut into N equal segments, one per rank):
//
//      collective_bench                  N = 2 and 4, 512 MiB, best of 3
//      collective_bench --mb 64 --ranks 2,3,5 --reps 5 --sockbuf 4096 --segment 4096   (KiB)
//
//  Loopback is not the network. It measures what the implementation costs -
//  the system calls, the copies through the kernel, the reduction - with a
//  "wire" of memory bandwidth, shared by every rank on one machine. On a
//  1 Gbit/s network the wire is the limit, so the benchmark ends with what
//  that network allows, computed from the bytes each
//  rank sends: 2 (N-1)/N of the buffer per allreduce, both directions of every
//  link busy at once.
//

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "net/collective.hpp"

namespace {

using namespace std::chrono_literals;
namespace net = snicholls::net;
using clock_type = std::chrono::steady_clock;

int sockbuf = 0;               // --sockbuf KiB: SO_SNDBUF/SO_RCVBUF; 0 leaves the kernel's default
std::size_t segment = 0;       // --segment KiB: the reduction's unit; 0 is the default (1 MiB)

struct timing {
    double allreduce_s = 0, broadcast_s = 0, bf16_s = 0, a2a_s = 0;
};

// One rank: connect, then the best of `reps` of each collective (after one warm-up)
timing rank_main(const net::collective_config& cfg, net::tcp_listener listener, std::size_t n, int reps)
{
    auto made = net::collective::connect(cfg, std::move(listener));
    if (made.is_err()) {
        std::cerr << "rank " << cfg.rank << ": " << made.unwrap_err_unchecked().what() << "\n";
        std::exit(1);
    }
    auto& c = *made;
    std::vector<float> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = float(i % 1024) * 0.001f + float(cfg.rank);
    std::vector<std::uint16_t> h(n);   // the same element count in bfloat16: half the bytes
    for (std::size_t i = 0; i < n; ++i) h[i] = std::uint16_t(0x3f80 + (i % 128));
    std::vector<float> got(n);         // the all-to-all's: n / N elements to and from each rank
    const std::size_t N = cfg.peers.size(), each = n / N;
    std::vector<std::size_t> counts(N, each), offsets(N);
    for (std::size_t q = 0; q < N; ++q) offsets[q] = q * each;
    timing best{1e9, 1e9, 1e9, 1e9};
    for (int rep = 0; rep <= reps; ++rep) {
        if (c.barrier().is_err()) std::exit(1);
        auto t = clock_type::now();
        if (auto e = c.allreduce_sum(v.data(), n); e.is_err()) {
            std::cerr << e.unwrap_err_unchecked().what() << "\n";
            std::exit(1);
        }
        const double a = std::chrono::duration<double>(clock_type::now() - t).count();
        if (c.barrier().is_err()) std::exit(1);
        t = clock_type::now();
        if (auto e = c.broadcast(v.data(), n, 0); e.is_err()) {
            std::cerr << e.unwrap_err_unchecked().what() << "\n";
            std::exit(1);
        }
        const double b = std::chrono::duration<double>(clock_type::now() - t).count();
        if (c.barrier().is_err()) std::exit(1);
        t = clock_type::now();
        if (auto e = c.allreduce_sum_bf16(h.data(), n); e.is_err()) {
            std::cerr << e.unwrap_err_unchecked().what() << "\n";
            std::exit(1);
        }
        const double f = std::chrono::duration<double>(clock_type::now() - t).count();
        if (c.barrier().is_err()) std::exit(1);
        t = clock_type::now();
        if (auto e = c.all_to_all_v(v.data(), counts, offsets, got.data(), counts, offsets); e.is_err()) {
            std::cerr << e.unwrap_err_unchecked().what() << "\n";
            std::exit(1);
        }
        const double x = std::chrono::duration<double>(clock_type::now() - t).count();
        if (rep > 0) {                        // rep 0 warms the page tables and the sockets
            best.allreduce_s = std::min(best.allreduce_s, a);
            best.broadcast_s = std::min(best.broadcast_s, b);
            best.bf16_s = std::min(best.bf16_s, f);
            best.a2a_s = std::min(best.a2a_s, x);
        }
    }
    return best;
}

// N processes; each reports its best times through a pipe; the slowest rank's time is the collective's
bool run(int N, std::size_t n, int reps, timing& out)
{
    std::vector<net::tcp_listener> ls;
    std::vector<std::string> peers;
    for (int r = 0; r < N; ++r) {
        ls.push_back(net::tcp_listener::bind("127.0.0.1", 0));
        peers.push_back("127.0.0.1:" + std::to_string(ls.back().port()));
    }
    int fds[2];
    if (::pipe(fds) != 0) return false;
    std::cout << std::flush;
    std::vector<pid_t> kids;
    for (int r = 0; r < N; ++r) {
        const pid_t pid = ::fork();
        if (pid == 0) {
            ::close(fds[0]);
            for (int k = 0; k < N; ++k)
                if (k != r) ls[std::size_t(k)].close();
            net::collective_config cfg;
            cfg.rank = r;
            cfg.peers = peers;
            cfg.run_id = 7;
            if (sockbuf) cfg.socket_buffer_bytes = sockbuf;
            if (segment) cfg.segment_bytes = segment;
            const timing t = rank_main(cfg, std::move(ls[std::size_t(r)]), n, reps);
            const ssize_t w = ::write(fds[1], &t, sizeof t);
            ::_exit(w == ssize_t(sizeof t) ? 0 : 1);
        }
        kids.push_back(pid);
    }
    ::close(fds[1]);
    for (auto& l : ls) l.close();
    out = {};
    timing t;
    while (::read(fds[0], &t, sizeof t) == ssize_t(sizeof t)) {
        out.allreduce_s = std::max(out.allreduce_s, t.allreduce_s);
        out.broadcast_s = std::max(out.broadcast_s, t.broadcast_s);
        out.bf16_s = std::max(out.bf16_s, t.bf16_s);
        out.a2a_s = std::max(out.a2a_s, t.a2a_s);
    }
    ::close(fds[0]);
    bool ok = true;
    for (const pid_t k : kids) {
        int status = 0;
        ::waitpid(k, &status, 0);
        ok = ok && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    std::size_t mb = 512;
    int reps = 3;
    std::vector<int> ranks{2, 4};
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        if (a == "--mb") mb = std::size_t(std::atol(argv[i + 1]));
        else if (a == "--sockbuf") sockbuf = std::atoi(argv[i + 1]) * 1024;
        else if (a == "--segment") segment = std::size_t(std::atol(argv[i + 1])) * 1024;
        else if (a == "--reps") reps = std::max(1, std::atoi(argv[i + 1]));
        else if (a == "--ranks") {
            ranks.clear();
            std::stringstream s(argv[i + 1]);
            for (std::string x; std::getline(s, x, ',');) ranks.push_back(std::atoi(x.c_str()));
        }
    }
    const std::size_t n = mb * 1024 * 1024 / sizeof(float);
    const double bytes = double(n) * sizeof(float);

    std::printf("collectives on loopback: %zu MiB of floats (%zu elements), each rank a process, best of %d\n", mb, n,
                reps);
    std::printf("algbw = buffer / time; busbw = bytes each rank sends / time (2(N-1)/N of the buffer for allreduce,\n"
                "(N-1)/N for all_to_all_v)\n\n");
    std::printf("| ranks | allreduce_sum | algbw GB/s | busbw GB/s | broadcast | algbw GB/s | allreduce_sum_bf16, same n "
                "| all_to_all_v | busbw GB/s |\n");
    std::printf("|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n");
    for (const int N : ranks) {
        timing t;
        if (!run(N, n, reps, t)) {
            std::printf("| %d | failed |\n", N);
            return 1;
        }
        const double sent = 2.0 * (N - 1) / N * bytes;
        const double moved = double(N - 1) / N * bytes;
        std::printf("| %d | %.3f s | %.2f | %.2f | %.3f s | %.2f | %.3f s | %.3f s | %.2f |\n", N, t.allreduce_s,
                    bytes / t.allreduce_s / 1e9, sent / t.allreduce_s / 1e9, t.broadcast_s, bytes / t.broadcast_s / 1e9, t.bf16_s,
                    t.a2a_s, moved / t.a2a_s / 1e9);
    }

    // What 1 Gbit/s allows: TCP over Ethernet carries about 0.94 Gbit/s of payload (1500-byte
    // frames), so ~117 MB/s each way per link, and every rank sends and receives at once
    const double goodput = 0.94e9 / 8;
    std::printf("\nloopback is not the network. On 10 machines at 1 Gbit/s (~%.0f MB/s of TCP payload each way),\n"
                "each rank sends 2 (N-1)/N = 1.8 x the buffer per allreduce:\n\n", goodput / 1e6);
    std::printf("| model | parameters | buffer | sent per rank | allreduce at 1 Gbit/s |\n|---|---:|---:|---:|---:|\n");
    const struct {
        const char* name;
        double params;
    } models[] = {{"GPT-2 small", 124e6}, {"GPT-2 medium", 350e6}};
    for (const auto& m : models) {
        const double buf = m.params * 4;
        const double sent = 2.0 * 9 / 10 * buf;
        std::printf("| %s | %.0fM | %.2f GB | %.2f GB | %.1f s float, %.1f s bfloat16 |\n", m.name, m.params / 1e6, buf / 1e9,
                    sent / 1e9, sent / goodput, sent / 2 / goodput);
    }
    return 0;
}
