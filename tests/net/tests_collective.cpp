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
//    - allreduce_sum_bf16 on 1, 2, 3 and 5 ranks equals the reference (each
//      bfloat16 decoded, summed in rank order in float, rounded once to
//      bfloat16), bit for bit, NaN and infinities included; a float and a
//      bfloat16 call with the same n are a mismatched call
//    - elastic membership: a rank that leaves between collectives is seen by
//      poll(), the survivors reform() and continue; a rank killed in the middle
//      of an allreduce (a process), the same; the killed slot's process starts
//      again, join()s and is admitted at admit(); the rendezvous itself (slot 0)
//      leaving; a joiner with another run id refused - and every collective in
//      every epoch is the same bits on every member and the reference for that
//      epoch's membership
//

#include "../test_helpers.hpp"

#include <algorithm>
#include <atomic>
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

#include <snicholls/core/ml/minifloat.hpp>

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

// bfloat16 bits of values(): the same magnitudes, rounded to nearest even, and a few specials
std::vector<std::uint16_t> bf16_values(int rank, std::size_t n, std::uint64_t seed = 2026)
{
    const auto f = values(rank, n, seed);
    std::vector<std::uint16_t> v(n);
    for (std::size_t i = 0; i < n; ++i)
        v[i] = snicholls::ml::to_bfloat16(f[i]).bits;
    if (n > 40 && rank == 1) {
        v[7] = 0x7f80;    // +inf
        v[8] = 0xff80;    // -inf (with +inf from rank 2 below: NaN)
        v[9] = 0x7fc1;    // a quiet NaN with a payload
        v[10] = 0x7f81;   // a signalling NaN: quieted by the reduction
        v[11] = 0x8000;   // -0
    }
    if (n > 40 && rank == 2)
        v[8] = 0x7f80;
    return v;
}

// The specification of the bfloat16 all-reduce: bf16(((f(x_0) + f(x_1)) + ...) + f(x_{N-1})), f exact decoding
std::vector<std::uint16_t> bf16_reference(const std::vector<std::vector<std::uint16_t>>& xs)
{
    namespace ml = snicholls::ml;
    std::vector<std::uint16_t> out(xs[0].size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        float s = ml::to_float(ml::bfloat16::from_bits(xs[0][i]));
        for (std::size_t k = 1; k < xs.size(); ++k)
            s = s + ml::to_float(ml::bfloat16::from_bits(xs[k][i]));
        out[i] = ml::to_bfloat16(s).bits;
    }
    return out;
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

void check_allreduce_bf16(int n_ranks, std::size_t n, std::size_t segment_bytes = 0)
{
    std::vector<std::vector<std::uint16_t>> xs;
    for (int r = 0; r < n_ranks; ++r) xs.push_back(bf16_values(r, n));
    const auto expect = bf16_reference(xs);
    if (n_ranks == 1) {
        collective_config c;
        c.peers = {"127.0.0.1:9"};
        auto one = collective::connect(c);
        assert(one.is_ok() && one->allreduce_sum_bf16(xs[0].data(), n).is_ok() && xs[0] == expect);
        return;
    }
    auto j = make_job(n_ranks);
    if (segment_bytes)
        for (auto& c : j.cfgs) c.segment_bytes = segment_bytes;
    std::vector<int> good(std::size_t(n_ranks), 0);
    run_ranks(j, [&](int r, collective_result<collective>& c) {
        if (c.is_err()) { std::cerr << c.unwrap_err_unchecked().what() << "\n"; return; }
        auto& data = xs[std::size_t(r)];
        const auto res = c->allreduce_sum_bf16(data.data(), data.size());
        if (res.is_err()) { std::cerr << "rank " << r << ": " << res.unwrap_err_unchecked().what() << "\n"; return; }
        good[std::size_t(r)] = data == expect ? 1 : 0;
    });
    for (int r = 0; r < n_ranks; ++r) assert(good[std::size_t(r)] == 1);
}

void test_allreduce_bf16()
{
    {   // the specials do what float addition of the decoded values says, then the rounding
        std::vector<std::vector<std::uint16_t>> xs;
        for (int r = 0; r < 3; ++r) xs.push_back(bf16_values(r, 100));
        const auto e = bf16_reference(xs);
        assert((e[7] & 0x7fff) > 0x7f80 || e[7] == 0x7f80);    // inf + finite
        assert((e[8] & 0x7fff) > 0x7f80);                       // -inf + inf: NaN
        assert((e[10] & 0x7fff) > 0x7f80 && (e[10] & 0x0040));  // a NaN comes back quiet
        // and the order is the rank order: summed the other way round, many elements differ
        std::size_t differ = 0;
        auto big = xs;
        for (int r = 0; r < 3; ++r) big[std::size_t(r)] = bf16_values(r, 100000);
        const auto fwd = bf16_reference(big);
        auto rev = big;
        std::reverse(rev.begin(), rev.end());
        const auto back = bf16_reference(rev);
        for (std::size_t i = 0; i < fwd.size(); ++i) differ += fwd[i] != back[i];
        std::cout << "      summed in reverse, " << differ << " of 100000 bfloat16 sums differ\n";
        assert(differ > 10);
    }
    for (std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{100}})
        check_allreduce_bf16(1, n);
    for (int N : {2, 3, 5})
        for (std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t(N - 1), std::size_t(N + 1), std::size_t{1001},
                              std::size_t{1'000'003}})
            check_allreduce_bf16(N, n);
    check_allreduce_bf16(3, 100'003, 64);   // 32 values a segment: the ring wraps thousands of times
    check_allreduce_bf16(4, 9'999, 4);      // two values a segment
    {   // a float call and a bfloat16 call of the same n
        auto j = make_job(2);
        std::vector<collective_errc> code(2);
        run_ranks(j, [&](int r, collective_result<collective>& c) {
            assert(c.is_ok());
            std::vector<float> f(64, 1.0f);
            std::vector<std::uint16_t> h(64, 0x3f80);
            const auto res = r == 0 ? c->allreduce_sum(f.data(), f.size()) : c->allreduce_sum_bf16(h.data(), h.size());
            code[std::size_t(r)] = res.is_err() ? res.unwrap_err_unchecked().code() : collective_errc{};
        });
        assert(code[0] == collective_errc::mismatched_call && code[1] == collective_errc::mismatched_call);
    }
    pass("collective: allreduce_sum_bf16 on 1, 2, 3, 4, 5 ranks is bf16 of the rank-order float sum, bit for bit; "
         "float against bfloat16 is a mismatched call");
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
    // The error must come from the socket, not the clock: the stall deadline is far beyond the bound asserted below,
    // which also covers the child allocating its buffer before it dies (seconds on a slow shared CI runner)
    for (auto& c : j.cfgs) c.io_timeout = 60s;
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
            std::cout << "      rank " << r << " after " << took[std::size_t(r)] << " s: " << res.unwrap_err_unchecked().what() << "\n";
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
        assert(took[std::size_t(r)] < 30.0);   // half the stall deadline: the socket, not io_timeout, ended it
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


// A one-way throttle between two ranks, as an ssh tunnel over a slow link is: it reads eagerly from
// the dialling side (so that side's sends complete at once, into its buffer) and trickles the bytes
// on at `rate` bytes a second; the other direction is not slowed
struct throttle {
    tcp_listener front = tcp_listener::bind("127.0.0.1", 0);
    std::uint16_t back = 0;
    std::size_t rate = 0;
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;

    throttle(std::uint16_t to, std::size_t bytes_per_second) : back(to), rate(bytes_per_second)
    {
        ts.emplace_back([this] {
            while (!stop) {
                auto a = front.accept(50ms);
                if (!a.valid()) continue;
                auto b = snicholls::net::tcp_stream::connect("127.0.0.1", back, 2s);
                ts.emplace_back([this, a = std::move(a), b = std::move(b)]() mutable { pump(a, b); });
            }
        });
    }
    void pump(snicholls::net::tcp_stream& slow_in, snicholls::net::tcp_stream& out)
    {
        std::string q;                                         // in flight, unbounded
        const std::size_t tick = std::max<std::size_t>(1, rate / 100);
        auto next = clock_type::now();
        char buf[65536];
        while (!stop) {
            pollfd p[2] = {{slow_in.fd(), POLLIN, 0}, {out.fd(), POLLIN, 0}};
            ::poll(p, 2, 5);
            if (p[0].revents & (POLLIN | POLLHUP)) {
                const auto n = ::recv(slow_in.fd(), buf, sizeof buf, MSG_DONTWAIT);
                if (n == 0 && q.empty()) return;
                if (n > 0) q.append(buf, std::size_t(n));
            }
            if (p[1].revents & (POLLIN | POLLHUP)) {           // the fast direction, as it comes
                const auto n = ::recv(out.fd(), buf, sizeof buf, MSG_DONTWAIT);
                if (n == 0) return;
                if (n > 0 && !slow_in.write_all(buf, std::size_t(n), 2s)) return;
            }
            if (!q.empty() && clock_type::now() >= next) {
                const std::size_t k = std::min(tick, q.size());
                if (!out.write_all(q.data(), k, 2s)) return;
                q.erase(0, k);
                next += 10ms;
                if (next < clock_type::now()) next = clock_type::now();
            }
        }
    }
    ~throttle()
    {
        stop = true;
        for (std::size_t i = 0; i < ts.size(); ++i) ts[i].join();
    }
};

// Rank 1's sends to rank 0 cross a link of 2 MB/s that buffers: rank 1 finishes each collective
// long before rank 0 has received it, enters the next and waits on rank 0 for longer than
// io_timeout although nothing has stalled. Rank 0's progress notices keep rank 1's deadline off.
void test_slow_link_is_not_a_stall()
{
    auto j = make_job(2, 66);
    throttle t(j.listeners[0].port(), 2'000'000);
    j.cfgs[1].peers[0] = "127.0.0.1:" + std::to_string(t.front.port());
    for (auto& c : j.cfgs) c.io_timeout = 400ms;
    const std::size_t n = 1'000'000;                         // 2 MB a direction a collective: about 1 s on the link
    std::vector<int> good(2, 0);
    run_ranks(j, [&](int r, collective_result<collective>& c) {
        assert(c.is_ok());
        bool ok = true;
        for (std::uint64_t round = 0; round < 2 && ok; ++round) {
            std::vector<std::vector<float>> xs{values(0, n, round), values(1, n, round)};
            auto mine = xs[std::size_t(r)];
            const auto res = c->allreduce_sum(mine.data(), mine.size());
            if (res.is_err()) std::cerr << "      rank " << r << ": " << res.unwrap_err_unchecked().what() << "\n";
            ok = res.is_ok() && same_bits(mine, rank_order_sum(xs));
            const auto b = c->barrier();
            if (b.is_err()) std::cerr << "      rank " << r << " barrier: " << b.unwrap_err_unchecked().what() << "\n";
            ok = ok && b.is_ok();
        }
        good[std::size_t(r)] = ok;
    });
    assert(good[0] == 1 && good[1] == 1);
    pass("collective: a slow, buffering link (each collective ~1 s, io_timeout 400 ms) is progress, not a stall");
}

// ------------------------------------------------------------- elastic

std::vector<std::vector<float>> slot_values(const std::vector<std::uint32_t>& members, std::size_t n, std::uint64_t round)
{
    std::vector<std::vector<float>> xs;
    for (const auto s : members) xs.push_back(values(int(s), n, 100 + round));
    return xs;
}

collective_config elastic_cfg(const job& j, int slot)
{
    auto c = j.cfgs[std::size_t(slot)];
    c.elastic = true;
    c.io_timeout = 10s;
    c.probe_timeout = 500ms;
    c.gather_window = 5s;
    c.reform_timeout = 20s;
    c.join_timeout = 20s;
    return c;
}

// One member's allreduce of its slot's values for this round, checked against the reference for the membership
bool round_ok(collective& c, std::size_t n, std::uint64_t round)
{
    const auto xs = slot_values(c.members(), n, round);
    auto mine = xs[std::size_t(c.rank())];
    const auto res = c.allreduce_sum(mine.data(), mine.size());
    if (res.is_err()) {
        std::cerr << "      slot " << c.slot() << ": " << res.unwrap_err_unchecked().what() << "\n";
        return false;
    }
    return same_bits(mine, rank_order_sum(xs));
}

// Members call admit() at round boundaries, poll() between them, until a joiner is admitted
bool admit_until_joined(collective& c, std::size_t want)
{
    for (int k = 0; k < 400; ++k) {
        if (c.poll().is_err())
            return false;
        const auto a = c.admit();
        if (a.is_err()) {
            std::cerr << "      slot " << c.slot() << " admit: " << a.unwrap_err_unchecked().what() << "\n";
            return false;
        }
        if (*a > 0)
            return std::size_t(c.size()) == want;
        std::this_thread::sleep_for(25ms);
    }
    return false;
}

void test_elastic_leave_reform_rejoin()
{
    auto j = make_job(3, 77);
    const auto port2 = j.listeners[2].port();
    std::vector<int> good(3, 0);
    std::vector<std::thread> ts;
    std::atomic<int> reformed{0};
    for (int r = 0; r < 3; ++r)
        ts.emplace_back([&, r] {
            auto made = collective::connect(elastic_cfg(j, r), std::move(j.listeners[std::size_t(r)]));
            assert(made.is_ok());
            auto c = std::move(made).unwrap();
            bool ok = c.epoch() == 0 && c.size() == 3 && round_ok(c, 1001, 0);
            if (r == 2) {
                c = collective{};                       // leaves: every socket closed
                while (reformed.load() < 2) std::this_thread::sleep_for(10ms);
                // A joiner of another run is refused by the rendezvous
                auto wrong = elastic_cfg(j, 2);
                wrong.run_id = 78;
                auto bad = collective::join(wrong, tcp_listener::bind("127.0.0.1", port2));
                ok = ok && bad.is_err() && bad.unwrap_err_unchecked().code() == collective_errc::handshake_rejected;
                // The same slot, restarted: joins, and is admitted at the next admit()
                auto back = collective::join(elastic_cfg(j, 2), tcp_listener::bind("127.0.0.1", port2));
                if (back.is_err()) std::cerr << "      join: " << back.unwrap_err_unchecked().what() << "\n";
                ok = ok && back.is_ok();
                if (ok) {
                    auto& b = *back;
                    ok = b.epoch() == 2 && b.size() == 3 && b.rank() == 2 && round_ok(b, 1001, 2) && round_ok(b, 7, 3);
                }
                good[2] = ok;
                return;
            }
            // The survivors see the departure between collectives, and re-form
            collective_result<void> p = snicholls::ok();
            for (int k = 0; k < 200 && (p = c.poll()).is_ok(); ++k) std::this_thread::sleep_for(10ms);
            ok = ok && p.is_err() && c.broken();
            const auto f = c.reform();
            if (f.is_err()) std::cerr << "      slot " << r << " reform: " << f.unwrap_err_unchecked().what() << "\n";
            ok = ok && f.is_ok() && c.epoch() == 1 && c.size() == 2 && c.members() == std::vector<std::uint32_t>{0, 1};
            ok = ok && round_ok(c, 1001, 1);
            reformed.fetch_add(1);
            ok = ok && admit_until_joined(c, 3) && c.epoch() == 2;
            ok = ok && round_ok(c, 1001, 2) && round_ok(c, 7, 3);
            good[std::size_t(r)] = ok;
        });
    for (auto& t : ts) t.join();
    for (int r = 0; r < 3; ++r) assert(good[std::size_t(r)] == 1);
    pass("collective: elastic - a rank leaves, poll() sees it, the survivors reform; a joiner of another run is "
         "refused; the slot rejoins at admit(); every epoch's allreduce is its membership's reference");
}

void test_elastic_rendezvous_leaves()
{
    auto j = make_job(4, 91);
    std::vector<int> good(4, 0);
    std::vector<std::thread> ts;
    for (int r = 0; r < 4; ++r)
        ts.emplace_back([&, r] {
            auto made = collective::connect(elastic_cfg(j, r), std::move(j.listeners[std::size_t(r)]));
            assert(made.is_ok());
            auto c = std::move(made).unwrap();
            bool ok = round_ok(c, 333, 0);
            if (r == 0) {                               // the rendezvous goes
                good[0] = ok;
                return;
            }
            ok = ok && !round_ok(c, 333, 1);            // its allreduce fails
            const auto f = c.reform();
            if (f.is_err()) std::cerr << "      slot " << r << " reform: " << f.unwrap_err_unchecked().what() << "\n";
            ok = ok && f.is_ok() && c.epoch() == 1 && c.members() == std::vector<std::uint32_t>{1, 2, 3} && c.rank() == r - 1;
            ok = ok && round_ok(c, 333, 1) && round_ok(c, 100'000, 2);
            good[std::size_t(r)] = ok;
        });
    for (auto& t : ts) t.join();
    for (int r = 0; r < 4; ++r) assert(good[std::size_t(r)] == 1);
    pass("collective: elastic - slot 0 leaves; slot 1 coordinates; ranks renumber densely in slot order");
}

// Processes: slot 2 is killed in the middle of an allreduce; slots 0 and 1 (threads here) re-form and
// continue; a second process for slot 2, forked at the start and waiting, then joins and is admitted
void test_elastic_killed_and_rejoined()
{
    auto j = make_job(3, 55);
    const std::size_t big = 20'000'000;
    int go[2];
    assert(::pipe(go) == 0);
    const pid_t killed = ::fork();
    assert(killed >= 0);
    if (killed == 0) {
        j.listeners[0].close();
        j.listeners[1].close();
        auto c = collective::connect(elastic_cfg(j, 2), std::move(j.listeners[2]));
        if (c.is_ok() && round_ok(*c, 1001, 0)) {
            std::vector<float> v(big, 1.0f);
            std::thread killer([] { std::this_thread::sleep_for(20ms); ::kill(::getpid(), SIGKILL); });
            (void)c->allreduce_sum(v.data(), v.size());
            ::kill(::getpid(), SIGKILL);
        }
        ::_exit(3);
    }
    const pid_t rejoiner = ::fork();
    assert(rejoiner >= 0);
    if (rejoiner == 0) {
        ::close(go[1]);
        j.listeners[0].close();
        j.listeners[1].close();
        char b = 0;
        if (::read(go[0], &b, 1) != 1)
            ::_exit(4);
        auto c = collective::join(elastic_cfg(j, 2), std::move(j.listeners[2]));   // the same bound port
        if (c.is_err()) {
            std::cerr << "      rejoiner: " << c.unwrap_err_unchecked().what() << "\n";
            ::_exit(5);
        }
        const bool ok = c->epoch() == 2 && c->size() == 3 && round_ok(*c, 1001, 2) && round_ok(*c, 99, 3);
        ::_exit(ok ? 0 : 6);
    }
    ::close(go[0]);
    j.listeners[2].close();
    std::vector<int> good(2, 0);
    std::vector<std::thread> ts;
    for (int r = 0; r < 2; ++r)
        ts.emplace_back([&, r] {
            auto made = collective::connect(elastic_cfg(j, r), std::move(j.listeners[std::size_t(r)]));
            assert(made.is_ok());
            auto c = std::move(made).unwrap();
            bool ok = round_ok(c, 1001, 0);
            std::vector<float> v(big, 1.0f);
            const auto res = c.allreduce_sum(v.data(), v.size());
            ok = ok && res.is_err() && res.unwrap_err_unchecked().peer() == 2;
            const auto f = c.reform();
            if (f.is_err()) std::cerr << "      slot " << r << " reform: " << f.unwrap_err_unchecked().what() << "\n";
            ok = ok && f.is_ok() && c.epoch() == 1 && c.size() == 2;
            ok = ok && round_ok(c, 1001, 1);
            if (r == 0) {
                int status = 0;
                ::waitpid(killed, &status, 0);
                ok = ok && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
                ok = ok && ::write(go[1], "g", 1) == 1;
            }
            ok = ok && admit_until_joined(c, 3) && c.epoch() == 2;
            ok = ok && round_ok(c, 1001, 2) && round_ok(c, 99, 3);
            good[std::size_t(r)] = ok;
        });
    for (auto& t : ts) t.join();
    ::close(go[1]);
    int status = 0;
    ::waitpid(rejoiner, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(good[0] == 1 && good[1] == 1);
    pass("collective: elastic - a process killed mid-allreduce; the survivors reform and continue; a new process for "
         "its slot joins and is admitted; every epoch's allreduce is its membership's reference");
}

void test_elastic_requires_elastic()
{
    auto j = make_job(2);
    run_ranks(j, [&](int, collective_result<collective>& c) {
        assert(c.is_ok());
        assert(c->reform().unwrap_err_unchecked().code() == collective_errc::bad_config);
        assert(c->admit().unwrap_err_unchecked().code() == collective_errc::bad_config);
        assert(c->poll().is_ok() && c->epoch() == 0 && c->members() == (std::vector<std::uint32_t>{0, 1}));
    });
    pass("collective: reform() and admit() need an elastic collective; poll() is a no-op without one");
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
    run("elastic_killed_and_rejoined", test_elastic_killed_and_rejoined);   // forks too
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
    run("allreduce_bf16", test_allreduce_bf16);
    run("slow_link_is_not_a_stall", test_slow_link_is_not_a_stall);
    run("elastic_requires_elastic", test_elastic_requires_elastic);
    run("elastic_leave_reform_rejoin", test_elastic_leave_reform_rejoin);
    run("elastic_rendezvous_leaves", test_elastic_rendezvous_leaves);
    std::cout << "\ncollective: all " << tests_run << " tests passed\n";
    return 0;
}
