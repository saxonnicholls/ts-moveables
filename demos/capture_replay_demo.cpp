//
//  capture_replay_demo.cpp
//  TSMoveables
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - event capture and replay over a node topology
//
//  The high-frequency-trading discipline: journal every ingress event, and be
//  able to replay the journal through the same topology to reproduce every
//  downstream state, bit for bit. This demo builds a multi-hop node graph on
//  moveable_signal and proves three things:
//
//    1. contention & overhead - capture is one 24-byte append to a reserved
//       journal; partitioned pipelines on 4 threads share nothing and scale
//    2. liveness - every event traverses a 3-hop reentrant emission chain
//       (slot emits into slot into slot); a lock-held-during-emission design
//       deadlocks or serialises here, snapshot emission cannot
//    3. accuracy - per-stream sequence monotonicity is asserted at every hop,
//       and replaying the journal reproduces the exact egress stream hash
//
//  Build and run:   make demo-capture           (compiled -O3 -DNDEBUG)
//  Bigger runs:     ./build/capture_replay_demo 4     (scales 4x)
//

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

#include "../TSMoveables/moveable_signal.hpp"

using snicholls::moveable_signal;
using snicholls::scoped_connection;

namespace {

// ---------------------------------------------------------------- the event

struct MarketEvent {
    std::int64_t seq;
    std::int32_t symbol;
    std::int32_t qty;
    double price;
};
static_assert(sizeof(MarketEvent) == 24);

// FNV-1a over the event fields - the determinism fingerprint of a stream
struct stream_hash {
    std::uint64_t h = 14695981039346656037ULL;
    void mix64(std::uint64_t v) noexcept {
        h ^= v;
        h *= 1099511628211ULL;
    }
    void mix(const MarketEvent& e) noexcept {
        mix64(static_cast<std::uint64_t>(e.seq));
        mix64((static_cast<std::uint64_t>(static_cast<std::uint32_t>(e.symbol)) << 32) |
              static_cast<std::uint32_t>(e.qty));
        std::uint64_t p;
        std::memcpy(&p, &e.price, sizeof p);
        mix64(p);
    }
};

// ---------------------------------------------------------------- the nodes

// Forwards its partition of the flow, enriching the price; re-emits from
// inside the slot - hop one of the reentrant chain
struct normalizer {
    int partition = 0, of = 1;
    moveable_signal<const MarketEvent&> out;
    scoped_connection in;
    long long seen = 0;
    std::int64_t last_seq = -1;
    bool order_ok = true;

    void attach(moveable_signal<const MarketEvent&>& src) {
        in = src.connect([this](const MarketEvent& e) {
            if (e.symbol % of != partition)
                return;
            if (e.seq <= last_seq)
                order_ok = false;               // per-stream FIFO, checked per hop
            last_seq = e.seq;
            ++seen;
            MarketEvent n = e;
            n.price *= 1.0001;
            out(n);                             // emit-from-within-slot
        });
    }
};

// Fan-in from every normalizer; re-emits - hop two
struct aggregator {
    moveable_signal<const MarketEvent&> out;
    std::vector<scoped_connection> ins;
    long long seen = 0;

    void attach(normalizer& n) {
        ins.emplace_back(n.out.connect([this](const MarketEvent& e) {
            ++seen;
            out(e);
        }));
    }
};

// Terminal node - hop three: order check plus the stream fingerprint
struct egress {
    scoped_connection in;
    stream_hash hash;
    long long count = 0;
    std::int64_t last_seq = -1;
    bool order_ok = true;

    void attach(moveable_signal<const MarketEvent&>& src) {
        in = src.connect([this](const MarketEvent& e) {
            if (e.seq <= last_seq)
                order_ok = false;               // global order survives 3 hops
            last_seq = e.seq;
            hash.mix(e);
            ++count;
        });
    }
};

// The journal: capture is one push_back into reserved storage
struct journal_node {
    std::vector<MarketEvent> log;
    scoped_connection in;

    void attach(moveable_signal<const MarketEvent&>& src, std::size_t reserve) {
        log.reserve(reserve);
        in = src.connect([this](const MarketEvent& e) { log.push_back(e); });
    }
};

// ------------------------------------------------------------- the topology

// feed -> 4 normalizers (partitioned) -> aggregator -> egress, with an
// optional ingress journal on the feed
struct pipeline {
    moveable_signal<const MarketEvent&> feed;
    std::array<normalizer, 4> normalizers;
    aggregator agg;
    egress sink;
    journal_node journal;

    explicit pipeline(std::size_t journal_reserve = 0) {
        if (journal_reserve != 0)
            journal.attach(feed, journal_reserve);  // capture first: ingress order is canonical
        for (int i = 0; i < 4; ++i) {
            normalizers[static_cast<std::size_t>(i)].partition = i;
            normalizers[static_cast<std::size_t>(i)].of = 4;
            normalizers[static_cast<std::size_t>(i)].attach(feed);
            agg.attach(normalizers[static_cast<std::size_t>(i)]);
        }
        sink.attach(agg.out);
    }

    bool ordered() const {
        bool ok = sink.order_ok;
        for (const auto& n : normalizers)
            ok = ok && n.order_ok;
        return ok;
    }
};

MarketEvent make_event(std::int64_t i) noexcept {
    return MarketEvent{i, static_cast<std::int32_t>(i % 16),
                       static_cast<std::int32_t>(1 + i % 100),
                       100.0 + static_cast<double>(i % 1000) * 0.01};
}

// --------------------------------------------------------------- plumbing

bool markdown = false;                      // --markdown: emit a GitHub-flavoured table

void check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::exit(1);
    }
}

double seconds_since(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

void report(const char* name, long long events, double s)
{
    if (markdown)
        std::printf("| %s | %.1f M events/s |\n", name, static_cast<double>(events) / s / 1e6);
    else
        std::printf("  %-52s %9.1f M events/s\n", name, static_cast<double>(events) / s / 1e6);
}

} // namespace

int main(int argc, char** argv)
{
    long long scale = 1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--markdown") == 0)
            markdown = true;
        else if (argv[i][0] != '-')             // ignore other flags for forward compatibility
            scale = std::atoll(argv[i]);
    }
    const long long N = 5'000'000 * (scale > 0 ? scale : 1);

    if (markdown)
        std::printf("### `make demo-capture` - capture/replay topology, %lldM events per scenario\n\n"
                    "| Scenario | Result |\n|---|---|\n",
                    static_cast<long long>(N / 1'000'000));
    else
        std::printf("capture/replay demo - %lld ingress events per scenario\n\n",
                    static_cast<long long>(N));

    // ---------------------------------------------- A. baseline, no capture
    double baseline_s;
    std::uint64_t live_hash;
    {
        pipeline p;
        const auto t0 = std::chrono::steady_clock::now();
        for (long long i = 0; i < N; ++i)
            p.feed(make_event(i));
        baseline_s = seconds_since(t0);
        check(p.sink.count == N, "baseline delivered every event");
        check(p.ordered(), "baseline order preserved at every hop");
        live_hash = p.sink.hash.h;
        report("pipeline, 3 hops, no capture", N, baseline_s);
    }

    // ---------------------------------------------- B. live run with capture
    std::vector<MarketEvent> journal;
    {
        pipeline p(static_cast<std::size_t>(N));
        const auto t0 = std::chrono::steady_clock::now();
        for (long long i = 0; i < N; ++i)
            p.feed(make_event(i));
        const double s = seconds_since(t0);
        check(p.sink.count == N, "captured run delivered every event");
        check(p.ordered(), "captured run order preserved at every hop");
        check(p.sink.hash.h == live_hash, "identical inputs give identical egress (determinism)");
        check(p.journal.log.size() == static_cast<std::size_t>(N), "journal captured every ingress event");
        report("pipeline with ingress capture", N, s);
        const double overhead_ns = (s - baseline_s) / static_cast<double>(N) * 1e9;
        if (markdown)
            std::printf("| capture overhead (journal append) | %.1f ns/event |\n", overhead_ns);
        else
            std::printf("  %-52s %9.1f ns/event\n", "capture overhead (journal append)", overhead_ns);
        journal = std::move(p.journal.log);
    }

    // ---------------------------------------------- C. replay from the journal
    {
        pipeline p;                             // a fresh topology
        const auto t0 = std::chrono::steady_clock::now();
        for (const MarketEvent& e : journal)
            p.feed(e);
        const double s = seconds_since(t0);
        check(p.sink.count == N, "replay delivered every event");
        check(p.ordered(), "replay order preserved at every hop");
        check(p.sink.hash.h == live_hash, "replay reproduces the exact egress stream hash");
        report("replay from journal", N, s);
    }

    // ---------------------------------------------- D. 4 partitioned pipelines,
    // 4 threads, private journals, plus a shared monitor node every pipeline
    // samples into - cross-topology edges while running concurrently
    {
        static constexpr int n_parts = 4;
        moveable_signal<const MarketEvent&> monitor;
        std::atomic<long long> monitored{0};
        scoped_connection mon = monitor.connect(
            [&monitored](const MarketEvent&) { monitored.fetch_add(1, std::memory_order_relaxed); });

        std::array<pipeline, n_parts> parts{
            pipeline{static_cast<std::size_t>(N)}, pipeline{static_cast<std::size_t>(N)},
            pipeline{static_cast<std::size_t>(N)}, pipeline{static_cast<std::size_t>(N)}};

        const auto t0 = std::chrono::steady_clock::now();
        std::vector<std::thread> threads;
        for (int t = 0; t < n_parts; ++t)
            threads.emplace_back([&parts, &monitor, t, N] {
                pipeline& p = parts[static_cast<std::size_t>(t)];
                for (long long i = 0; i < N; ++i) {
                    const MarketEvent e = make_event(i);
                    p.feed(e);
                    if (i % 1024 == 0)
                        monitor(e);             // concurrent emission into a shared node
                }
            });
        for (auto& t : threads)
            t.join();
        const double s = seconds_since(t0);

        std::array<std::uint64_t, n_parts> hashes{};
        for (int t = 0; t < n_parts; ++t) {
            pipeline& p = parts[static_cast<std::size_t>(t)];
            check(p.sink.count == N, "partition delivered every event");
            check(p.ordered(), "partition order preserved");
            hashes[static_cast<std::size_t>(t)] = p.sink.hash.h;
        }
        check(monitored.load() == static_cast<long long>(n_parts) * ((N + 1023) / 1024),
              "shared monitor saw every sampled event, no deadlock");
        report("4 partitioned pipelines on 4 threads (total)", N * n_parts, s);
        const double scaling =
            (static_cast<double>(N * n_parts) / s) / (static_cast<double>(N) / baseline_s);
        if (markdown)
            std::printf("| scaling vs single pipeline (shared-nothing) | %.2fx |\n", scaling);
        else
            std::printf("  %-52s %9.2fx\n", "scaling vs single pipeline (shared-nothing)", scaling);

        // Replay each partition's journal on one thread - all hashes reproduce
        const auto t1 = std::chrono::steady_clock::now();
        for (int t = 0; t < n_parts; ++t) {
            pipeline fresh;
            for (const MarketEvent& e : parts[static_cast<std::size_t>(t)].journal.log)
                fresh.feed(e);
            check(fresh.sink.hash.h == hashes[static_cast<std::size_t>(t)],
                  "partition replay reproduces its egress hash");
        }
        report("replay of all 4 partition journals (1 thread)", N * n_parts, seconds_since(t1));
    }

    const double rate = static_cast<double>(N) / baseline_s;
    if (markdown) {
        std::printf("\nOrdering asserted at every hop, live and replayed; egress hashes matched "
                    "exactly; ~%.1f billion events/hour at the baseline rate.\n",
                    rate * 3600.0 / 1e9);
        return 0;
    }
    std::printf("\n  every event crossed 3 reentrant hops; ordering asserted at every hop,\n"
                "  on every run, live and replayed; egress hashes matched exactly.\n");
    std::printf("  at the baseline rate this topology sustains %.1f billion events/hour\n",
                rate * 3600.0 / 1e9);
    return 0;
}
