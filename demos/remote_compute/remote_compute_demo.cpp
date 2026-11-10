//
//  remote_compute_demo.cpp
//  TSMoveables - demos/remote_compute (from the remote_compute_example prototype at 92d8dc4)
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Remote std::async / std::invoke over this library's TCP and thread pools:
//  named kernels, a compile-time perfect-hash dispatch, a field-wise binary
//  wire format, mutual HMAC-SHA256 authentication, sessions that survive a
//  reconnect, long jobs with progress and cancel, health pings and UDP
//  discovery. C++20 (class-type template parameters) and POSIX sockets; the
//  library itself stays C++17. `make demo-remote` runs the self-test - every
//  worker in this process on 127.0.0.1 - which is what CI runs.
//
//    remote_demo keygen
//        Write a new shared key to ~/.snicholls_remote_key. Copy that file to
//        every machine: workers and clients must hold the same key.
//
//    remote_demo selftest
//        Workers in this process on loopback, every behaviour checked. Exits
//        non-zero on any failure.
//
//    remote_demo worker <name> [port] [bind]
//        Serve the kernels on this machine (default port 7400, bind 0.0.0.0)
//        and announce it on the LAN, until Ctrl-C. Bind 127.0.0.1 on a machine
//        on the public internet and reach it through an SSH tunnel.
//
//    remote_demo run [<name>=<host>:<port> ...]
//        Spread a Monte Carlo price across those workers, or with no arguments,
//        across every worker heard on the LAN in three seconds.
//
//    remote_demo farm <name>[=<host>:<port>] [paths]
//        Send one long job to one machine - found on the LAN if no address is
//        given - and watch its progress and health. Ctrl-C cancels.
//

#include "kernels.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <future>
#include <set>
#include <string>
#include <vector>

using namespace example;
namespace r = snicholls::remote;
using namespace std::chrono_literals;

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

template <typename F>
std::string error_of(F&& f)
{
    try {
        f();
    }
    catch (const r::remote_error& e) {
        return e.what();
    }
    catch (const std::exception& e) {
        return std::string("WRONG TYPE: ") + e.what();
    }
    return "";
}

bool contains(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

// The same job, split in `chunks`, combined in chunk order
mc_chunk combine(std::vector<std::future<mc_chunk>>& parts)
{
    mc_chunk total;
    for (auto& p : parts) {
        const auto c = p.get();
        total.sum += c.sum;
        total.sum_sq += c.sum_sq;
        total.paths += c.paths;
    }
    return total;
}

int selftest(bool lan)
{
    std::printf("remote compute self-test: two workers on loopback\n");

    // Compile-time dispatch: every name lands in its own slot, nothing else does
    static_assert(kernels::find("price_chunk") == 0);
    static_assert(kernels::find("scale") == 5);
    static_assert(kernels::find("nope") == -1);
    static_assert(kernels::find("") == -1);
    std::printf("  perfect hash: %zu kernels in %zu slots, seed %llu\n", kernels::size, kernels::slots,
                static_cast<unsigned long long>(kernels::seed));

    const std::string key = r::random_bytes(32);   // every machine in a cluster shares one
    r::worker<kernels> studio(key, "mac-studio", "127.0.0.1", 0, 4);
    auto pi = std::make_unique<r::worker<kernels>>(key, "pi-3", "127.0.0.1", 0, 2);

    r::cluster<kernels> c(key);
    c.add("mac-studio", "127.0.0.1", studio.port());
    c.add("pi-3", "127.0.0.1", pi->port());
    check(c.nodes().size() == 2, "both workers connected and said hello");

    // 1. A remote result is the local result, bit for bit
    const option_spec spec{};
    const auto here = price_chunk(spec, 7, 20000);
    const auto there = c.async(r::on{"pi-3"}, price_chunk, spec, 7u, 20000u).get();
    check(here.sum == there.sum && here.sum_sq == there.sum_sq && here.paths == there.paths,
          "remote Monte Carlo chunk equals the local one bit for bit");

    // 2. Placement by name
    check(c.async(r::on{"pi-3"}, whoami).get().name == "pi-3", "on(\"pi-3\") runs on pi-3");
    check(c.async(r::on{"mac-studio"}, whoami).get().name == "mac-studio", "on(\"mac-studio\") runs on mac-studio");

    // 3. Placement by requirement, round robin
    check(c.async(r::any{.min_cores = 3, .arch = ""}, whoami).get().name == "mac-studio",
          "any{min_cores = 3} picks the 4-thread worker, not the 2-thread one");
    std::set<std::string> seen;
    for (int i = 0; i < 4; ++i)
        seen.insert(c.async(r::any{}, whoami).get().name);
    check(seen.size() == 2, "any{} spreads calls across both workers");

    // 3b. invoke: the synchronous form, like std::invoke
    check(c.invoke(r::on{"pi-3"}, add, 40, 2) == 42, "invoke() runs remotely and returns the value");
    check(contains(error_of([&] { c.invoke(r::on{"pi-3"}, must_fail, std::string("bad input")); }), "bad input"),
          "invoke() throws the remote exception, as std::invoke propagates the callee's");

    // 4. Local placement runs here, no network
    check(c.async(r::local{}, count_primes, 0u, 100u).get() == 25, "local{} runs in this process");

    // 5. Containers cross the wire
    check(c.async(r::any{}, scale, std::vector<double>{1, 2, 3}, 2.0).get() == std::vector<double>{2, 4, 6},
          "a vector argument and result round-trip");

    // 6. A kernel that throws: the message arrives through get()
    {
        auto f = c.async(r::on{"pi-3"}, must_fail, std::string("disk full"));
        const auto e = error_of([&] { f.get(); });
        check(contains(e, "disk full") && contains(e, "pi-3"),
              "a kernel's exception arrives as remote_error naming the worker and the message");
    }

    // 7. A big job spread across both machines equals one machine doing it all
    {
        std::vector<std::future<mc_chunk>> remote_parts, local_parts;
        for (std::uint64_t i = 0; i < 16; ++i) {
            remote_parts.push_back(c.async(r::any{}, price_chunk, spec, 1000 + i, std::uint64_t{25000}));
            local_parts.push_back(std::async(std::launch::deferred, price_chunk, spec, 1000 + i, std::uint64_t{25000}));
        }
        const auto a = combine(remote_parts), b = combine(local_parts);
        const double price = a.sum / double(a.paths);
        std::printf("  16 chunks, 400k paths over 2 workers: call price %.4f\n", price);
        check(a.sum == b.sum && a.paths == b.paths, "split across the cluster == computed here, bit for bit");
        check(price > 10.0 && price < 11.0, "and it is the Black-Scholes price (10.45) to Monte Carlo accuracy");
    }

    // 8. remote_thread joins
    {
        const auto t0 = std::chrono::steady_clock::now();
        {
            auto t = c.spawn(r::on{"mac-studio"}, sleep_ms, 150u);
        }
        check(std::chrono::steady_clock::now() - t0 >= 150ms, "remote_thread joins in its destructor");
    }

    // 9. The worker refuses what it was not built with
    check(contains(c.call_raw("mac-studio", "launch_missiles", 0, "").get(), "no kernel called"),
          "an unknown kernel name is refused");
    check(contains(c.call_raw("mac-studio", "count_primes", 0x1234, "").get(), "different signature"),
          "a signature mismatch (a different build) is refused");
    check(error_of([&] { c.async(r::on{"nowhere"}, whoami); }).find("no machine called") != std::string::npos,
          "an unknown machine name throws at the call");

    // 10. A machine dies with a call outstanding: at-most-once, so the call fails
    {
        auto slow = c.async(r::on{"pi-3"}, sleep_ms, 2000u);
        std::this_thread::sleep_for(100ms);
        std::thread killer([&] { pi.reset(); });   // stop() cuts the links, then waits for the kernel
        const auto e = error_of([&] { slow.get(); });
        check(contains(e, "lost"), "a worker dying mid-call fails the future with 'connection lost'");
        killer.join();
        check(c.nodes().size() == 1, "the dead worker leaves the cluster's live set");
        check(c.async(r::any{}, whoami).get().name == "mac-studio", "and work goes to the survivor");
    }

    // 11. A long job reports progress as it runs, and the answer is unchanged
    {
        const std::uint64_t paths = 20'000'000;   // about a second: long enough for several throttled updates
        auto j = c.submit(r::on{"mac-studio"}, long_price, spec, paths);
        std::vector<double> seen_progress;
        while (!j.done()) {
            const auto p = j.progress();
            if (p.at != r::steady::time_point{} && (seen_progress.empty() || p.fraction != seen_progress.back()))
                seen_progress.push_back(p.fraction);
            std::this_thread::sleep_for(20ms);
        }
        const auto result = j.get();
        const auto here = long_price(spec, paths);
        check(seen_progress.size() >= 2, "a long job's progress arrives while it runs");
        check(std::is_sorted(seen_progress.begin(), seen_progress.end()), "and only goes forward");
        check(j.progress().fraction == 1.0, "and ends at 100%");
        check(result.sum == here.sum, "and the result is still bit for bit the local one");
        check(j.node_name() == "mac-studio", "the job knows which machine it is on");
    }

    // 12. Cancel: the kernel stops at its next check and get() says so
    {
        auto j = c.submit(r::on{"mac-studio"}, long_price, spec, std::uint64_t{2'000'000'000});
        std::this_thread::sleep_for(150ms);
        const auto t0 = std::chrono::steady_clock::now();
        j.cancel();
        bool cancelled = false;
        try {
            j.get();
        }
        catch (const r::job_cancelled&) {
            cancelled = true;
        }
        check(cancelled, "cancel() stops a running job and get() throws job_cancelled");
        // The job would run ~100 s to the end; 5 s leaves a slow CI runner room
        // and still fails if cancel waited for the end
        check(std::chrono::steady_clock::now() - t0 < 5s, "within one block of work, not at the end");
    }

    // 13. Health: a worker with every thread busy still answers a ping at once
    {
        std::vector<std::future<void>> busy;
        for (int i = 0; i < 4; ++i)
            busy.push_back(c.async(r::on{"mac-studio"}, sleep_ms, 800u));
        std::this_thread::sleep_for(150ms);
        const auto t0 = std::chrono::steady_clock::now();
        const auto h = c.ping("mac-studio");
        const auto took = std::chrono::steady_clock::now() - t0;
        // The jobs hold every thread for ~650 ms more; a ping that waited for
        // them would take that long. 400 ms is slack for a slow CI runner.
        check(h.has_value() && took < 400ms, "a ping is answered at once while every worker thread is busy");
        check(h && h->first.running == 4 && h->first.cores == 4, "and reports 4 of 4 threads running jobs");
        for (auto& f : busy)
            f.get();
        std::this_thread::sleep_for(1200ms);   // a heartbeat or two
        const auto all = c.health();
        check(!all.empty() && all.front().health.has_value(), "the heartbeat keeps cluster health() current");
    }

    // 14. A machine that stops answering pings has its jobs failed, not left hanging
    {
        auto silent = snicholls::net::tcp_listener::bind("127.0.0.1", 0);
        std::thread fake([&] {
            // Pass the handshake like a real worker, then answer nothing at all
            auto s = silent.accept(2s);
            const std::string nonce = r::random_bytes(32);
            std::string hello;
            r::encode(hello, r::node_info{"frozen", 1, "x86-64", "nowhere"}, nonce);
            const auto h = r::detail::frame_bytes(r::frame_kind::hello, "frozen", r::detail::protocol_signature, hello);
            s.write_all(h.data(), h.size(), 1s);
            std::string buf;
            r::frame f;
            std::size_t used = 0;
            r::detail::read_frame(s, buf, f, used, 2s);
            r::reader ar(f.payload);
            ar.get<std::uint64_t>();
            const auto client_nonce = ar.get<std::string>();
            std::string welcome;
            r::encode(welcome, r::detail::server_proof(key, nonce, client_nonce), std::vector<std::uint64_t>{});
            const auto w = r::detail::frame_bytes(r::frame_kind::welcome, "frozen", r::detail::protocol_signature, welcome);
            s.write_all(w.data(), w.size(), 1s);
            char sink_buf[4096];
            const auto until = std::chrono::steady_clock::now() + 3s;
            while (std::chrono::steady_clock::now() < until)   // read everything, answer nothing
                s.read(sink_buf, sizeof sink_buf, 100ms);
        });
        r::cluster<kernels> frozen(key);
        frozen.add("frozen", "127.0.0.1", silent.port(),
                   r::link_options{.connect_timeout = 300ms, .ping_every = 100ms, .unresponsive_after = 600ms,
                                   .resume_window = 400ms, .first_retry = 100ms, .max_retry = 200ms});
        auto f = frozen.async(r::on{"frozen"}, add, 1, 2);
        const auto t0 = std::chrono::steady_clock::now();
        const auto e = error_of([&] { f.get(); });
        check(contains(e, "unresponsive") && contains(e, "did not come back"),
              "a worker that stops answering pings fails its jobs as unresponsive, after the resume window");
        // Configured: ~1 s (600 ms silence + 400 ms resume window). The
        // defaults would be 10 s + 60 s. 10 s tells the two apart even on a
        // slow CI runner, where 2.5 s did not.
        check(std::chrono::steady_clock::now() - t0 < 10s, "after the configured silence, not minutes later");
        fake.join();
    }

    // 15. Authentication: the wrong key gets nowhere, in either direction
    {
        const std::string other = r::random_bytes(32);
        const auto refused = error_of([&] {
            r::cluster<kernels> intruder(other);
            intruder.add("mac-studio", "127.0.0.1", studio.port());
        });
        check(contains(refused, "authentication failed"), "a client without the key is refused by the worker");

        // An impostor accepts any client and makes up its half of the proof
        auto fake_l = snicholls::net::tcp_listener::bind("127.0.0.1", 0);
        std::thread impostor([&] {
            auto s = fake_l.accept(2s);
            std::string hello;
            r::encode(hello, r::node_info{"mac-studio", 64, "x86-64", "evil"}, r::random_bytes(32));
            const auto h = r::detail::frame_bytes(r::frame_kind::hello, "mac-studio", r::detail::protocol_signature, hello);
            s.write_all(h.data(), h.size(), 1s);
            std::string buf;
            r::frame f;
            std::size_t used = 0;
            r::detail::read_frame(s, buf, f, used, 2s);
            std::string welcome;
            r::encode(welcome, r::random_bytes(32), std::vector<std::uint64_t>{});
            const auto w = r::detail::frame_bytes(r::frame_kind::welcome, "mac-studio", r::detail::protocol_signature, welcome);
            s.write_all(w.data(), w.size(), 1s);
        });
        const auto fooled = error_of([&] {
            r::cluster<kernels> victim(key);
            victim.add("mac-studio", "127.0.0.1", fake_l.port());
        });
        impostor.join();
        check(contains(fooled, "does not hold the shared key"), "a worker without the key is refused by the client");
    }

    // 16. The connection drops mid-job: the job keeps running, the client
    //     reconnects, and the result arrives - computed once, never re-run
    {
        r::cluster<kernels> rc(key);
        rc.add("mac-studio", "127.0.0.1", studio.port(), r::link_options{.first_retry = 50ms});
        const auto before = studio.health().completed;
        auto j = rc.submit(r::on{"mac-studio"}, long_price, spec, std::uint64_t{15'000'000});
        std::this_thread::sleep_for(150ms);
        studio.drop_connections();
        const auto result = j.get();
        check(result.sum == long_price(spec, 15'000'000).sum, "a job survives its connection dropping mid-run");
        check(rc.health().front().reconnects >= 1, "the client reconnected by itself");
        check(studio.health().completed == before + 1, "and the job ran exactly once");
    }

    // 17. A job finishes while the client is away: the worker keeps the result
    {
        r::cluster<kernels> rc(key);
        rc.add("mac-studio", "127.0.0.1", studio.port(), r::link_options{.first_retry = 700ms});
        auto f = rc.async(r::on{"mac-studio"}, add, 40, 2);
        auto slow = rc.async(r::on{"mac-studio"}, sleep_ms, 200u);
        std::this_thread::sleep_for(50ms);
        studio.drop_connections();   // the client stays away 700 ms; sleep_ms finishes after 200
        check(slow.wait_for(500ms) == std::future_status::timeout, "while away, the result cannot arrive");
        slow.get();
        check(f.get() == 42, "and after reconnecting, the kept result is delivered");
    }

    // 18. A call made while reconnecting waits for the connection and then runs
    {
        r::cluster<kernels> rc(key);
        rc.add("mac-studio", "127.0.0.1", studio.port(), r::link_options{.first_retry = 300ms});
        studio.drop_connections();
        std::this_thread::sleep_for(50ms);
        auto f = rc.async(r::on{"mac-studio"}, add, 1, 1);
        check(f.get() == 2, "a call made during a reconnect is sent once the connection is back");
    }

    // 19. Discovery: workers announce themselves; only beacons signed with our key count
    if (!lan)
        std::printf("  skip  discovery (selftest --no-lan: no multicast on this network)\n");
    else {
        r::discovery_options d;
        d.port = std::uint16_t(41000 + (std::chrono::steady_clock::now().time_since_epoch().count() % 2000));
        d.every = 200ms;
        // Listening on every interface, as real workers do: the beacon's source
        // address is the LAN interface, not loopback
        r::worker<kernels> loud(key, "announced", "0.0.0.0", 0, 1);
        r::worker<kernels> stranger(r::random_bytes(32), "stranger", "0.0.0.0", 0, 1);
        loud.advertise(d);
        stranger.advertise(d);
        r::cluster<kernels> dc(key);
        std::atomic<int> heard{0};
        dc.discover(d, [&](const r::node_info&) { ++heard; });
        for (int i = 0; i < 40 && dc.nodes().empty(); ++i)
            std::this_thread::sleep_for(100ms);
        const auto found = dc.nodes();
        check(found.size() == 1 && found.front().name == "announced", "a worker announcing itself is found and added");
        std::this_thread::sleep_for(600ms);
        check(dc.nodes().size() == 1 && heard == 1, "a beacon signed with another key is ignored");
        check(dc.async(r::on{"announced"}, add, 2, 3).get() == 5, "and the discovered worker runs jobs");
    }

    std::printf(failures == 0 ? "self-test: ok\n" : "self-test: %d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}

volatile std::sig_atomic_t interrupted = 0;

std::string bar(double f)
{
    const int width = 30, filled = int(f * width + 0.5);
    return "[" + std::string(std::size_t(filled), '#') + std::string(std::size_t(width - filled), '.') + "]";
}

// Farm one long job out and watch it: progress, the machine's health, Ctrl-C cancels
// <name>=<host>:<port> adds directly; a bare <name> is waited for on the LAN
void add_or_find(r::cluster<kernels>& c, const std::string& arg)
{
    const auto eq = arg.find('='), colon = arg.rfind(':');
    if (eq != std::string::npos && colon != std::string::npos && colon > eq) {
        c.add(arg.substr(0, eq), arg.substr(eq + 1, colon - eq - 1), std::uint16_t(std::stoi(arg.substr(colon + 1))));
        return;
    }
    std::printf("listening for '%s' on the LAN...\n", arg.c_str());
    c.discover();
    for (int i = 0; i < 100; ++i) {
        for (const auto& n : c.nodes())
            if (n.name == arg)
                return;
        std::this_thread::sleep_for(100ms);
    }
    throw std::runtime_error("no worker called '" + arg + "' announced itself within 10 s");
}

int farm(const std::string& spec_arg, std::uint64_t paths)
{
    r::cluster<kernels> c(r::load_key());
    add_or_find(c, spec_arg);
    const std::string name = spec_arg.substr(0, spec_arg.find('='));
    std::signal(SIGINT, [](int) { interrupted = 1; });

    std::printf("farming %llu paths of Monte Carlo out to %s. Ctrl-C cancels.\n",
                static_cast<unsigned long long>(paths), name.c_str());
    auto j = c.submit(r::on{name}, long_price, option_spec{}, paths);
    bool cancel_sent = false;
    while (!j.done()) {
        std::this_thread::sleep_for(1s);
        if (interrupted && !cancel_sent) {
            std::printf("\n  cancelling...\n");
            j.cancel();
            cancel_sent = true;
        }
        const auto p = j.progress();
        const double secs = std::chrono::duration<double>(j.elapsed()).count();
        const double eta = p.fraction > 0 ? secs / p.fraction - secs : 0;
        std::string health = "no heartbeat yet";
        for (const auto& s : c.health())
            if (s.health)
                health = std::string(s.connected ? "alive" : s.reachable ? "RECONNECTING" : "LOST") +
                         (s.reconnects ? " (" + std::to_string(s.reconnects) + " reconnects)" : std::string()) +
                         ", ping " +
                         std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(s.rtt).count() / 1000.0)
                             .substr(0, 5) +
                         " ms, " + std::to_string(s.health->running) + "/" + std::to_string(s.health->cores) +
                         " threads busy, load " + std::to_string(s.health->load1).substr(0, 4);
        std::printf("  %s %5.1f%%  %6.0f s elapsed  eta %5.0f s  | %s\n", bar(p.fraction).c_str(),
                    100 * p.fraction, secs, eta, health.c_str());
        if (!p.note.empty())
            std::printf("      %s\n", p.note.c_str());
    }
    try {
        const auto total = j.get();
        std::printf("done on %s: call price %.5f from %llu paths in %.1f s\n", j.node_name().c_str(),
                    total.sum / double(total.paths), static_cast<unsigned long long>(total.paths),
                    std::chrono::duration<double>(j.elapsed()).count());
    }
    catch (const r::job_cancelled& e) {
        std::printf("cancelled: %s\n", e.what());
    }
    return 0;
}

// Bound to 0.0.0.0 (the default) the worker is reachable on the LAN and announces
// itself there. Bound to 127.0.0.1 it is reachable only through an SSH tunnel -
// the way to run one on a machine on the public internet, since nothing here is
// encrypted - and it does not announce.
int serve(const std::string& name, std::uint16_t port, const std::string& bind)
{
    r::worker<kernels> w(r::load_key(), name, bind, port);
    const bool lan = bind != "127.0.0.1" && bind != "localhost";
    if (lan)
        w.advertise();
    std::printf("worker '%s' on %s:%u, %u threads, %s%s. Ctrl-C to stop.\n", name.c_str(), bind.c_str(),
                unsigned(w.port()), std::thread::hardware_concurrency(), r::detail::arch_name().c_str(),
                lan ? ", announcing on the LAN" : ", local only (reach it through an SSH tunnel)");
    std::signal(SIGINT, [](int) { interrupted = 1; });
    while (!interrupted)
        std::this_thread::sleep_for(200ms);
    return 0;
}

int run(int argc, char** argv)
{
    r::cluster<kernels> c(r::load_key());
    for (int i = 2; i < argc; ++i)
        add_or_find(c, argv[i]);
    if (argc == 2) {
        std::printf("listening for workers on the LAN for 3 s...\n");
        c.discover();
        std::this_thread::sleep_for(3s);
        if (c.nodes().empty())
            throw std::runtime_error("no workers announced themselves");
    }
    for (const auto& n : c.nodes())
        std::printf("  %-14s %2u cores  %-7s %s\n", n.name.c_str(), n.cores, n.arch.c_str(), n.host.c_str());

    const option_spec spec{};
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::future<mc_chunk>> parts;
    for (std::uint64_t i = 0; i < 64; ++i)
        parts.push_back(c.async(r::any{}, price_chunk, spec, 1000 + i, std::uint64_t{250000}));
    const auto total = combine(parts);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("call price %.5f from %llu paths in %.2f s across %zu machines\n", total.sum / double(total.paths),
                static_cast<unsigned long long>(total.paths), secs, c.nodes().size());
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // progress shows even if a check hangs
    try {
        if (argc >= 2 && std::strcmp(argv[1], "selftest") == 0)
            return selftest(!(argc >= 3 && std::strcmp(argv[2], "--no-lan") == 0));
        if (argc >= 3 && std::strcmp(argv[1], "worker") == 0)
            return serve(argv[2], argc >= 4 ? std::uint16_t(std::stoi(argv[3])) : std::uint16_t{7400},
                         argc >= 5 ? argv[4] : "0.0.0.0");
        if (argc >= 2 && std::strcmp(argv[1], "keygen") == 0) {
            r::generate_key_file();
            std::printf("wrote a new shared key to %s\ncopy it to every machine, e.g.  scp %s other-machine:\n",
                        r::key_path().c_str(), r::key_path().c_str());
            return 0;
        }
        if (argc >= 2 && std::strcmp(argv[1], "run") == 0)
            return run(argc, argv);
        if (argc >= 3 && std::strcmp(argv[1], "farm") == 0)
            return farm(argv[2], argc >= 4 ? std::stoull(argv[3]) : 400'000'000ull);
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    std::fprintf(stderr, "usage: %s keygen | selftest [--no-lan] | worker <name> [port] [bind] | run [<name>=<host>:<port> ...] | farm <name>[=<host>:<port>] [paths]\n", argv[0]);
    return 2;
}
