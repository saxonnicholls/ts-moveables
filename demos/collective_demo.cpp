//
//  collective_demo.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - data-parallel training's communication, as
//  separate processes on this machine or on several (net/collective.hpp):
//
//    1. connect: a full TCP mesh, every handshake checked
//    2. broadcast: rank 0's initial weights to every rank
//    3. five steps of: per-rank gradients, allreduce_sum, an SGD update
//    4. barrier, then a digest of the weights
//
//  Every rank checks everything it receives against what it can compute on
//  its own (every rank's gradients are a function of rank and step), bit for
//  bit, and exits non-zero on any difference: the weights stay identical on
//  every rank because the sum is the same rank-order sum everywhere.
//
//      collective_demo selftest [N]          N processes on 127.0.0.1 (default 4)
//      collective_demo rank R host:port,host:port,...   one rank of a real job
//
//  `make demo-collective` runs the self-test. For a real job, start the same
//  command on every machine with its own R and the same list in rank order.
//

#include <chrono>
#include <cmath>
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

constexpr std::size_t params = 1'000'003;     // not divisible by any N that matters
constexpr int steps = 5;
constexpr float lr = 0.01f;

// A gradient is a function of (rank, step, element), so every rank can compute every rank's
float gradient(int rank, int step, std::size_t i)
{
    std::uint64_t z = (std::uint64_t(rank) << 48) ^ (std::uint64_t(step) << 40) ^ i;
    z += 0x9e3779b97f4a7c15ull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    z ^= z >> 31;
    const float m = float(z & 0xffffff) / float(0x1000000) - 0.5f;
    return std::ldexp(m, int((z >> 24) % 17) - 8);   // magnitudes 2^-9..2^8: order matters
}

std::uint64_t digest(const std::vector<float>& v)
{
    std::uint64_t h = 1469598103934665603ull;
    for (const float x : v) {
        std::uint32_t b;
        std::memcpy(&b, &x, 4);
        h = (h ^ b) * 1099511628211ull;
    }
    return h;
}

bool same_bits(const std::vector<float>& a, const std::vector<float>& b)
{
    return std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

// One rank's whole run. Returns the process exit status.
int train(net::collective_config cfg, net::tcp_listener* listener)
{
    const int r = cfg.rank;
    const int N = int(cfg.peers.size());
    const auto t0 = clock_type::now();
    auto made = listener ? net::collective::connect(cfg, std::move(*listener)) : net::collective::connect(cfg);
    if (made.is_err()) {
        std::cerr << "rank " << r << ": " << made.unwrap_err_unchecked().what() << "\n";
        return 1;
    }
    auto& c = *made;
    const double connect_ms = std::chrono::duration<double, std::milli>(clock_type::now() - t0).count();

    // Initial weights: rank 0's, everywhere
    std::vector<float> w(params, 0.0f), expect(params);
    for (std::size_t i = 0; i < params; ++i) expect[i] = gradient(0, -1, i);
    if (r == 0) w = expect;
    if (auto b = c.broadcast(w.data(), w.size(), 0); b.is_err() || !same_bits(w, expect)) {
        std::cerr << "rank " << r << ": broadcast " << (b.is_err() ? b.unwrap_err_unchecked().what() : "wrong bits") << "\n";
        return 1;
    }

    double reduce_ms = 0;
    std::vector<float> g(params), sum(params);
    for (int step = 0; step < steps; ++step) {
        for (std::size_t i = 0; i < params; ++i) g[i] = gradient(r, step, i);
        // What every rank must end with: ((g_0 + g_1) + g_2) + ... in rank order
        for (std::size_t i = 0; i < params; ++i) {
            float s = gradient(0, step, i);
            for (int k = 1; k < N; ++k) s = s + gradient(k, step, i);
            sum[i] = s;
        }
        const auto t = clock_type::now();
        if (auto a = c.allreduce_sum(g.data(), g.size()); a.is_err()) {
            std::cerr << "rank " << r << ": allreduce_sum: " << a.unwrap_err_unchecked().what() << "\n";
            return 1;
        }
        reduce_ms += std::chrono::duration<double, std::milli>(clock_type::now() - t).count();
        if (!same_bits(g, sum)) {
            std::cerr << "rank " << r << ": step " << step << ": the sum is not the rank-order sum\n";
            return 1;
        }
        for (std::size_t i = 0; i < params; ++i) {
            w[i] -= lr * (g[i] / float(N));
            expect[i] -= lr * (sum[i] / float(N));
        }
    }
    if (auto b = c.barrier(); b.is_err() || !same_bits(w, expect)) {
        std::cerr << "rank " << r << ": barrier or weights differ\n";
        return 1;
    }

    // Each rank sends 2(N-1)/N of the buffer per allreduce
    const double sent_mb = 2.0 * (N - 1) / N * double(params) * 4 * steps / 1e6;
    std::ostringstream line;
    line << "  rank " << r << ": connected in " << int(connect_ms) << " ms; " << steps << " allreduces of " << params
         << " floats in " << int(reduce_ms) << " ms (" << int(sent_mb) << " MB sent); weights digest " << std::hex
         << digest(w) << "\n";
    std::cout << line.str() << std::flush;
    return 0;
}

int selftest(int N)
{
    std::cout << "collective demo: " << N << " processes on 127.0.0.1, " << params << " parameters, " << steps
              << " steps\n"
              << std::flush;                  // before fork, or every child prints it again
    // Listeners first, on ports the kernel picks; each child keeps its own
    std::vector<net::tcp_listener> ls;
    std::vector<std::string> peers;
    for (int r = 0; r < N; ++r) {
        ls.push_back(net::tcp_listener::bind("127.0.0.1", 0));
        if (!ls.back().valid()) {
            std::cerr << "bind failed\n";
            return 1;
        }
        peers.push_back("127.0.0.1:" + std::to_string(ls.back().port()));
    }
    std::vector<pid_t> kids;
    for (int r = 0; r < N; ++r) {
        const pid_t pid = ::fork();
        if (pid < 0) {
            std::perror("fork");
            return 1;
        }
        if (pid == 0) {
            for (int k = 0; k < N; ++k)
                if (k != r) ls[std::size_t(k)].close();
            net::collective_config cfg;
            cfg.rank = r;
            cfg.peers = peers;
            cfg.run_id = 0xdeadbeef;
            cfg.connect_timeout = 20s;
            cfg.io_timeout = 20s;
            ::_exit(train(cfg, &ls[std::size_t(r)]));
        }
        kids.push_back(pid);
    }
    for (auto& l : ls) l.close();
    int failed = 0;
    for (const pid_t k : kids) {
        int status = 0;
        ::waitpid(k, &status, 0);
        failed += !(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    if (failed) {
        std::cout << "FAILED: " << failed << " of " << N << " ranks\n";
        return 1;
    }
    std::cout << "every rank: the broadcast and every sum bit-identical to the rank-order reference; the same weights\n";
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    const std::string mode = argc > 1 ? argv[1] : "selftest";
    if (mode == "selftest") {
        const int N = argc > 2 ? std::atoi(argv[2]) : 4;
        if (N < 1 || N > 64) {
            std::cerr << "selftest takes 1 to 64 ranks\n";
            return 2;
        }
        return selftest(N);
    }
    if (mode == "rank" && argc > 3) {
        net::collective_config cfg;
        cfg.rank = std::atoi(argv[2]);
        std::stringstream list(argv[3]);
        for (std::string p; std::getline(list, p, ',');) cfg.peers.push_back(p);
        cfg.run_id = 0xdeadbeef;
        return train(cfg, nullptr);
    }
    std::cerr << "usage: collective_demo selftest [N]\n"
                 "       collective_demo rank R host:port,host:port,...\n";
    return 2;
}
