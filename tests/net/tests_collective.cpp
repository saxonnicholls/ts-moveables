//
//  tests_collective.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - tests for net/collective.hpp. Their own program
//  (C++20 and POSIX, like fs/), run by `make test-collective`. Ranks are
//  threads on 127.0.0.1, except the killed peer, which is a process:
//
//    - allreduce_sum with 1, 2, 3 and 5 ranks over 0, 1, N-1, N+1 and 10M+7
//      floats equals the sequential rank-order sum bit for bit on every rank,
//      with values whose sum provably depends on the order of addition
//    - the same with tiny segments, so the reduction ring wraps thousands of times
//    - broadcast from every root; barrier holds every rank until the last arrives
//    - several collectives in a row on one mesh
//    - a different run id, a different world size, a stray connection
//    - a rank calling a different collective, or with a different n
//    - a peer that never calls: timed_out naming it, within the deadline
//    - a killed peer process: an error naming it, not a hang
//

#include "../test_helpers.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "net/collective.hpp"

namespace {

using namespace std::chrono_literals;
using snicholls::net::collective;
using snicholls::net::collective_config;
using snicholls::net::collective_errc;
using snicholls::net::collective_result;
using snicholls::net::tcp_listener;
using clock_type = std::chrono::steady_clock;

// --------------------------------------------------------------- the data

std::uint64_t splitmix(std::uint64_t& s)
{
    std::uint64_t z = (s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

// Signs and magnitudes from 2^-20 to 2^20: float addition of these is far from associative
std::vector<float> values(int rank, std::size_t n, std::uint64_t seed = 2026)
{
    std::uint64_t s = seed * 1000003u + std::uint64_t(rank);
    std::vector<float> v(n);
    for (auto& x : v) {
        const std::uint64_t z = splitmix(s);
        const float m = float(z & 0xffffff) / float(0x1000000);
        const int e = int((z >> 24) % 41) - 20;
        x = std::ldexp((z >> 40) & 1 ? -m : m, e);
    }
    return v;
}

// The specification: per element ((x_0 + x_1) + x_2) + ... in rank order, in float
std::vector<float> rank_order_sum(const std::vector<std::vector<float>>& xs)
{
    std::vector<float> s = xs[0];
    for (std::size_t k = 1; k < xs.size(); ++k)
        for (std::size_t i = 0; i < s.size(); ++i)
            s[i] = s[i] + xs[k][i];
    return s;
}

bool same_bits(const std::vector<float>& a, const std::vector<float>& b)
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

// ---------------------------------------------------------------- a job

struct job {
    std::vector<collective_config> cfgs;
    std::vector<tcp_listener> listeners;
};

// N ranks on 127.0.0.1, each with a listener already bound to a port the kernel chose
job make_job(int n, std::uint64_t run_id = 42)
{
    job j;
    std::vector<std::string> peers;
    for (int r = 0; r < n; ++r) {
        j.listeners.push_back(tcp_listener::bind("127.0.0.1", 0));
        assert(j.listeners.back().valid());
        peers.push_back("127.0.0.1:" + std::to_string(j.listeners.back().port()));
    }
    for (int r = 0; r < n; ++r) {
        collective_config c;
        c.rank = r;
        c.peers = peers;
        c.run_id = run_id;
        c.connect_timeout = 10s;
        c.io_timeout = 10s;
        j.cfgs.push_back(c);
    }
    return j;
}

// One thread per rank: connect, then body(rank, the connect result)
void run_ranks(job& j, const std::function<void(int, collective_result<collective>&)>& body)
{
    std::vector<std::thread> ts;
    for (int r = 0; r < int(j.cfgs.size()); ++r)
        ts.emplace_back([&j, &body, r] {
            auto c = collective::connect(j.cfgs[std::size_t(r)], std::move(j.listeners[std::size_t(r)]));
            body(r, c);
        });
    for (auto& t : ts) t.join();
}

// ---------------------------------------------------------------- tests

void test_config_errors()
{
    collective_config c;
    assert(collective::connect(c).unwrap_err_unchecked().code() == collective_errc::bad_config);   // no peers
    c.peers = {"127.0.0.1:1", "127.0.0.1:2"};
    c.rank = 2;
    assert(collective::connect(c).unwrap_err_unchecked().code() == collective_errc::bad_config);   // no such rank
    c.rank = 0;
    for (const char* bad : {"127.0.0.1", "127.0.0.1:0", "127.0.0.1:70000", ":80", "[::1:80", "a:b:80", "h:8x"}) {
        c.peers = {bad, "127.0.0.1:2"};
        const auto r = collective::connect(c);
        assert(r.is_err() && r.unwrap_err_unchecked().code() == collective_errc::bad_config);
    }
    c.peers = {"[::1]:7100"};                       // a v6 literal parses; one rank needs no network
    assert(collective::connect(c).is_ok());

    collective idle;                                // never connected
    assert(idle.barrier().unwrap_err_unchecked().code() == collective_errc::bad_config);
    pass("collective: bad configuration is refused before any socket");
}

void test_chunks()
{
    for (std::size_t n : {0u, 1u, 4u, 5u, 7u, 1000u})
        for (std::size_t parts : {1u, 2u, 3u, 5u}) {
            std::size_t at = 0;
            for (std::size_t c = 0; c < parts; ++c) {
                assert(collective::chunk_begin(n, parts, c) == at);
                const auto len = collective::chunk_len(n, parts, c);
                assert(len == n / parts || len == n / parts + 1);
                at += len;
            }
            assert(at == n);
        }
    static_assert(collective::chunk_len(10, 3, 0) == 4 && collective::chunk_begin(10, 3, 2) == 7);
    pass("collective: chunks tile the buffer, the first n % N one longer");
}

void test_one_rank()
{
    collective_config c;
    c.peers = {"127.0.0.1:9"};
    auto r = collective::connect(c);
    assert(r.is_ok() && r->size() == 1 && r->rank() == 0);
    auto v = values(0, 1000);
    const auto before = v;
    assert(r->allreduce_sum(v.data(), v.size()).is_ok() && same_bits(v, before));
    assert(r->allreduce_sum(nullptr, 0).is_ok());
    assert(r->broadcast(v.data(), v.size(), 0).is_ok() && same_bits(v, before));
    assert(r->broadcast(v.data(), v.size(), 1).unwrap_err_unchecked().code() == collective_errc::bad_config);
    assert(r->barrier().is_ok());
    assert(r->sequence() == 4);
    pass("collective: one rank is the identity, with no network");
}

void check_allreduce(int n_ranks, std::size_t n, std::size_t segment_bytes = 0)
{
    std::vector<std::vector<float>> xs;
    for (int r = 0; r < n_ranks; ++r) xs.push_back(values(r, n));
    const auto expect = rank_order_sum(xs);

    if (n >= 100000 && n_ranks >= 3) {
        // The test can tell the right order from others: summed in reverse, or folded from the
        // right (x_0 + (x_1 + (... + x_{N-1}))), the bits differ for many elements
        std::size_t rev_diff = 0, right_diff = 0;
        for (std::size_t i = 0; i < n; ++i) {
            float rev = xs[std::size_t(n_ranks - 1)][i];
            for (int k = n_ranks - 2; k >= 0; --k) rev = rev + xs[std::size_t(k)][i];
            float right = xs[std::size_t(n_ranks - 1)][i];
            for (int k = n_ranks - 2; k >= 0; --k) right = xs[std::size_t(k)][i] + right;
            rev_diff += std::memcmp(&rev, &expect[i], 4) != 0;
            right_diff += std::memcmp(&right, &expect[i], 4) != 0;
        }
        assert(rev_diff > n / 100 && right_diff > n / 100);
    }

    auto j = make_job(n_ranks);
    if (segment_bytes)
        for (auto& c : j.cfgs) c.segment_bytes = segment_bytes;
    std::vector<int> good(std::size_t(n_ranks), 0);
    run_ranks(j, [&](int r, collective_result<collective>& c) {
        if (c.is_err()) { std::cerr << c.unwrap_err_unchecked().what() << "\n"; return; }
        auto& data = xs[std::size_t(r)];
        const auto res = c->allreduce_sum(data.data(), data.size());
        if (res.is_err()) { std::cerr << "rank " << r << ": " << res.unwrap_err_unchecked().what() << "\n"; return; }
        good[std::size_t(r)] = same_bits(data, expect) ? 1 : 0;
    });
    for (int r = 0; r < n_ranks; ++r) assert(good[std::size_t(r)] == 1);
}

void test_allreduce_small()
{
    for (int N : {2, 3, 5})
        for (std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t(N - 1), std::size_t(N + 1), std::size_t{1000}})
            check_allreduce(N, n);
    pass("collective: allreduce of 0, 1, N-1, N+1, 1000 floats on 2, 3, 5 ranks is the rank-order sum, bit for bit");
}

void test_allreduce_large()
{
    const std::size_t n = 10'000'000 + 7;
    for (int N : {2, 3, 5}) check_allreduce(N, n);
    pass("collective: allreduce of 10M+7 floats on 2, 3, 5 ranks is the rank-order sum on every rank, bit for bit");
}

void test_allreduce_ring_wraps()
{
    check_allreduce(3, 100'003, 64);        // 16-float segments: the two-segment ring wraps ~2,000 times
    check_allreduce(5, 99'999, 4);          // one float per segment
    pass("collective: allreduce with tiny segments (the ring wraps thousands of times) is still exact");
}

void test_broadcast()
{
    for (int N : {2, 3, 5})
        for (std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t(N - 1), std::size_t{1'000'003}})
            for (int root = 0; root < N; ++root) {
                const auto want = values(root, n, 7);
                auto j = make_job(N);
                std::vector<int> good(std::size_t(N), 0);
                run_ranks(j, [&](int r, collective_result<collective>& c) {
                    assert(c.is_ok());
                    auto data = r == root ? want : std::vector<float>(n, -1.0f);
                    const auto res = c->broadcast(data.data(), data.size(), root);
                    if (res.is_err()) { std::cerr << res.unwrap_err_unchecked().what() << "\n"; return; }
                    good[std::size_t(r)] = same_bits(data, want) ? 1 : 0;
                });
                for (int r = 0; r < N; ++r) assert(good[std::size_t(r)] == 1);
            }
    pass("collective: broadcast from every root on 2, 3, 5 ranks delivers the root's bits");
}

void test_barrier()
{
    auto j = make_job(3);
    clock_type::time_point late_entry{};
    std::vector<clock_type::time_point> left(3);
    run_ranks(j, [&](int r, collective_result<collective>& c) {
        assert(c.is_ok());
        if (r == 1) {
            std::this_thread::sleep_for(200ms);
            late_entry = clock_type::now();
        }
        assert(c->barrier().is_ok());
        left[std::size_t(r)] = clock_type::now();
    });
    for (const auto& t : left) assert(t >= late_entry);
    pass("collective: barrier holds every rank until the last one enters");
}

void test_sequence_of_collectives()
{
    const int N = 4;
    auto j = make_job(N);
    std::vector<int> good(N, 0);
    run_ranks(j, [&](int r, collective_result<collective>& c) {
        assert(c.is_ok());
        bool ok = true;
        for (int round = 0; round < 5; ++round) {
            const std::size_t n = 1000 + std::size_t(round) * 777;
            std::vector<std::vector<float>> xs;
            for (int k = 0; k < N; ++k) xs.push_back(values(k, n, std::uint64_t(round)));
            auto mine = xs[std::size_t(r)];
            ok = ok && c->allreduce_sum(mine.data(), mine.size()).is_ok() && same_bits(mine, rank_order_sum(xs));
            ok = ok && c->broadcast(mine.data(), mine.size(), round % N).is_ok() && same_bits(mine, rank_order_sum(xs));
            ok = ok && c->barrier().is_ok();
        }
        good[std::size_t(r)] = ok && c->sequence() == 15 && !c->broken();
    });
    for (int r = 0; r < N; ++r) assert(good[std::size_t(r)] == 1);
    pass("collective: fifteen collectives in a row on one mesh");
}

void test_run_id_mismatch()
{
    auto j = make_job(2);
    j.cfgs[1].run_id = 43;
    for (auto& c : j.cfgs) c.connect_timeout = 1500ms;
    std::vector<int> code(2, 0), peer(2, 9);
    run_ranks(j, [&](int r, collective_result<collective>& c) {
        assert(c.is_err());
        code[std::size_t(r)] = int(c.unwrap_err_unchecked().code());
        peer[std::size_t(r)] = c.unwrap_err_unchecked().peer();
    });
    assert(code[1] == int(collective_errc::handshake_rejected) && peer[1] == 0);   // told at once
    assert(code[0] == int(collective_errc::handshake_rejected) && peer[0] == 1);   // at its deadline, saying why
    pass("collective: a different run id is refused on both sides");
}

void test_world_mismatch()
{
    auto j = make_job(3);
    j.cfgs[1].peers.pop_back();          // rank 1 thinks the world is 2, so it is the last rank and only dials
    for (auto& c : j.cfgs) c.connect_timeout = 1500ms;
    std::vector<collective_errc> code(3);
    std::vector<std::thread> ts;
    for (int r = 0; r < 2; ++r)
        ts.emplace_back([&, r] {
            auto c = collective::connect(j.cfgs[std::size_t(r)], std::move(j.listeners[std::size_t(r)]));
            assert(c.is_err());
            code[std::size_t(r)] = c.unwrap_err_unchecked().code();
        });
    for (auto& t : ts) t.join();
    assert(code[1] == collective_errc::handshake_rejected && code[0] == collective_errc::handshake_rejected);
    pass("collective: a different world size is refused");
}

void test_stray_connection_is_ignored()
{
    auto j = make_job(2);
    const auto port = j.listeners[0].port();
    std::thread stray([port] {
        auto s = snicholls::net::tcp_stream::connect("127.0.0.1", port, 2s);
        const char junk[64] = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
        s.write_all(junk, sizeof junk, 1s);
        char buf[64];
        (void)s.read(buf, sizeof buf, 2s);
    });
    std::this_thread::sleep_for(50ms);   // the stray arrives first
    std::vector<int> good(2, 0);
    run_ranks(j, [&](int r, collective_result<collective>& c) {
        good[std::size_t(r)] = c.is_ok() && c->barrier().is_ok();
    });
    stray.join();
    assert(good[0] == 1 && good[1] == 1);
    pass("collective: a stray connection is refused and the job still connects");
}

void test_mismatched_calls()
{
    {
        auto j = make_job(2);
        std::vector<collective_errc> code(2);
        std::vector<int> peer(2), broken(2);
        run_ranks(j, [&](int r, collective_result<collective>& c) {
            assert(c.is_ok());
            std::vector<float> v(std::size_t(10 + r), 1.0f);
            const auto res = c->allreduce_sum(v.data(), v.size());
            assert(res.is_err());
            code[std::size_t(r)] = res.unwrap_err_unchecked().code();
            peer[std::size_t(r)] = res.unwrap_err_unchecked().peer();
            broken[std::size_t(r)] = c->barrier().unwrap_err_unchecked().code() == collective_errc::broken;
        });
        // Whichever reads the other's header first reports the mismatch and tells the other, which
        // reports the same even if the first one's close reset the connection before it read it
        for (int r = 0; r < 2; ++r)
            assert(code[std::size_t(r)] == collective_errc::mismatched_call && peer[std::size_t(r)] == 1 - r &&
                   broken[std::size_t(r)]);
    }
    {
        auto j = make_job(3);
        std::vector<collective_errc> code(3);
        run_ranks(j, [&](int r, collective_result<collective>& c) {
            assert(c.is_ok());
            std::vector<float> v(5, 1.0f);
            const auto res = r == 2 ? c->barrier() : c->broadcast(v.data(), v.size(), 0);
            code[std::size_t(r)] = res.is_err() ? res.unwrap_err_unchecked().code() : collective_errc{};
        });
        for (int r = 0; r < 3; ++r) assert(code[std::size_t(r)] == collective_errc::mismatched_call);
    }
    pass("collective: a different n or a different collective is a mismatched call, then broken");
}

void test_silent_peer_times_out()
{
    auto j = make_job(3);
    for (auto& c : j.cfgs) c.io_timeout = 400ms;
    std::vector<collective_errc> code(3);
    std::vector<int> peer(3, -1);
    std::vector<double> took(3);
    run_ranks(j, [&](int r, collective_result<collective>& c) {
        assert(c.is_ok());
        if (r == 2) {                    // alive, connected, never calls
            std::this_thread::sleep_for(2500ms);
            return;
        }
        std::vector<float> v(1'000'000, 1.0f);
        const auto t0 = clock_type::now();
        const auto res = c->allreduce_sum(v.data(), v.size());
        took[std::size_t(r)] = std::chrono::duration<double>(clock_type::now() - t0).count();
        assert(res.is_err());
        code[std::size_t(r)] = res.unwrap_err_unchecked().code();
        peer[std::size_t(r)] = res.unwrap_err_unchecked().peer();
    });
    for (int r = 0; r < 2; ++r) {
        assert(code[std::size_t(r)] == collective_errc::timed_out);
        assert(peer[std::size_t(r)] == 2);
        assert(took[std::size_t(r)] >= 0.35 && took[std::size_t(r)] < 2.0);
    }
    pass("collective: a peer that never calls is timed_out, named, within io_timeout");
}

void test_killed_peer()
{
    auto j = make_job(3);
    for (auto& c : j.cfgs) c.io_timeout = 10s;     // the error must come from the socket, not the clock
    const pid_t child = ::fork();
    assert(child >= 0);
    if (child == 0) {
        j.listeners[0].close();
        j.listeners[1].close();
        auto c = collective::connect(j.cfgs[2], std::move(j.listeners[2]));
        if (c.is_ok()) {
            std::vector<float> v(50'000'000, 1.0f);  // start the collective, then die in the middle of it
            std::thread killer([] { std::this_thread::sleep_for(20ms); ::kill(::getpid(), SIGKILL); });
            (void)c->allreduce_sum(v.data(), v.size());
            ::kill(::getpid(), SIGKILL);
        }
        ::_exit(3);
    }
    j.listeners[2].close();
    std::vector<collective_errc> code(2);
    std::vector<int> peer(2, -1);
    std::vector<double> took(2);
    std::vector<std::thread> ts;
    for (int r = 0; r < 2; ++r)
        ts.emplace_back([&, r] {
            auto c = collective::connect(j.cfgs[std::size_t(r)], std::move(j.listeners[std::size_t(r)]));
            assert(c.is_ok());
            std::vector<float> v(50'000'000, 1.0f);
            const auto t0 = clock_type::now();
            const auto res = c->allreduce_sum(v.data(), v.size());
            took[std::size_t(r)] = std::chrono::duration<double>(clock_type::now() - t0).count();
            assert(res.is_err());
            std::cout << "      rank " << r << ": " << res.unwrap_err_unchecked().what() << "\n";
            code[std::size_t(r)] = res.unwrap_err_unchecked().code();
            peer[std::size_t(r)] = res.unwrap_err_unchecked().peer();
        });
    for (auto& t : ts) t.join();
    int status = 0;
    ::waitpid(child, &status, 0);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    for (int r = 0; r < 2; ++r) {
        assert(code[std::size_t(r)] == collective_errc::peer_closed || code[std::size_t(r)] == collective_errc::io_error);
        assert(peer[std::size_t(r)] == 2);
        assert(took[std::size_t(r)] < 5.0);
    }
    pass("collective: a killed peer process is an error naming it, not a hang");
}

void test_connect_times_out()
{
    auto j = make_job(2);
    j.listeners[0].close();              // rank 0 never comes up
    j.cfgs[1].connect_timeout = 300ms;
    const auto t0 = clock_type::now();
    auto c = collective::connect(j.cfgs[1], std::move(j.listeners[1]));
    const double took = std::chrono::duration<double>(clock_type::now() - t0).count();
    assert(c.is_err() && c.unwrap_err_unchecked().code() == collective_errc::connect_failed);
    assert(c.unwrap_err_unchecked().peer() == 0 && took < 2.0);

    auto k = make_job(2);                // rank 1 never comes up: rank 0 waits connect_timeout, then names it
    k.cfgs[0].connect_timeout = 300ms;
    auto d = collective::connect(k.cfgs[0], std::move(k.listeners[0]));
    assert(d.is_err() && d.unwrap_err_unchecked().code() == collective_errc::timed_out);
    assert(d.unwrap_err_unchecked().peer() == 1);
    assert(std::string(d.unwrap_err_unchecked().what()).find("rank 1 (127.0.0.1:") != std::string::npos);
    pass("collective: a rank that never starts is named at connect_timeout");
}

} // namespace

int main(int argc, char** argv)
{
    // `tests_collective killed_peer` runs only the tests whose name contains the argument
    const std::string only = argc > 1 ? argv[1] : "";
    auto run = [&](const char* name, void (*f)()) {
        if (only.empty() || std::string(name).find(only) != std::string::npos) f();
    };
    run("config_errors", test_config_errors);
    run("chunks", test_chunks);
    run("one_rank", test_one_rank);
    run("killed_peer", test_killed_peer);                  // forks: first, while this process has one thread
    run("connect_times_out", test_connect_times_out);
    run("allreduce_small", test_allreduce_small);
    run("allreduce_ring_wraps", test_allreduce_ring_wraps);
    run("allreduce_large", test_allreduce_large);
    run("broadcast", test_broadcast);
    run("barrier", test_barrier);
    run("sequence_of_collectives", test_sequence_of_collectives);
    run("run_id_mismatch", test_run_id_mismatch);
    run("world_mismatch", test_world_mismatch);
    run("stray_connection_is_ignored", test_stray_connection_is_ignored);
    run("mismatched_calls", test_mismatched_calls);
    run("silent_peer_times_out", test_silent_peer_times_out);
    std::cout << "\ncollective: all " << tests_run << " tests passed\n";
    return 0;
}
