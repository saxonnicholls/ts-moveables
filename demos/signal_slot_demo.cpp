//
//  signal_slot_demo.cpp
//  TSMoveables
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - a comprehensive signal/slot demonstration
//
//  A typed publish/subscribe architecture built on snicholls::moveable_signal:
//  per-object-type emitters, virtual observers, multi-type emitters and
//  observers wired with fold expressions - and throughput measurements to
//  show the event rates and memory behaviour this design sustains.
//
//  Build and run:   make demo-signals            (compiled -O3 -DNDEBUG)
//  Bigger runs:     ./build/signal_slot_demo 10  (scales every scenario 10x)
//

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <utility>
#include <vector>

#include "../TSMoveables/moveable_signal.hpp"

// ---------------------------------------------------------------------------
// The typed publish/subscribe layer - the architecture, in ~60 lines.
//
// An object_emitter<O> owns one moveable_signal<const O&>. An
// object_observer<O> holds its subscription as a scoped_connection, so
// destroying an observer unsubscribes it automatically - no observer base
// class from the signal library required, no manual disconnect bookkeeping.
// multi_emitter / multi_observer compose any number of object types and wire
// them with fold expressions.
// ---------------------------------------------------------------------------

template <typename O>
struct object_emitter {
    snicholls::moveable_signal<const O&> on_event;

    void emit(const O& o) { on_event(o); }
};

template <typename O>
class object_observer {
    std::vector<snicholls::scoped_connection> subscriptions_;   // many-to-many: one observer, many emitters

public:
    virtual ~object_observer() = default;

    virtual void process_event(const O&) = 0;

    void subscribe_to(object_emitter<O>& e) {
        subscriptions_.emplace_back(e.on_event.connect([this](const O& o) { process_event(o); }));
    }
    void unsubscribe() { subscriptions_.clear(); }
    bool subscribed() const {
        for (const auto& s : subscriptions_)
            if (s.connected())
                return true;
        return false;
    }
};

// Emits any of Os... - emit() is a single overload set via a using-pack
template <typename... Os>
struct multi_emitter : object_emitter<Os>... {
    using object_emitter<Os>::emit...;

    template <typename T>
    object_emitter<T>& as() { return *this; }
};

// Observes any of Os... - one fold expression subscribes to all of them
template <typename... Os>
struct multi_observer : object_observer<Os>... {
    template <typename T>
    object_observer<T>& as() { return *this; }

    template <typename Emitter>
    void subscribe_all(Emitter& emitter) {
        (static_cast<object_observer<Os>&>(*this).subscribe_to(emitter.template as<Os>()), ...);
    }
    void unsubscribe_all() {
        (static_cast<object_observer<Os>&>(*this).unsubscribe(), ...);
    }
};

// Reusable observer flavours
template <typename O>
struct counting_observer : object_observer<O> {
    long long count = 0;
    void process_event(const O&) override { ++count; }
};

template <typename O>
struct concurrent_counting_observer : object_observer<O> {
    std::atomic<long long> count{0};
    void process_event(const O&) override { count.fetch_add(1, std::memory_order_relaxed); }
};

template <typename O>
struct stateful_observer : object_observer<O> {
    O state{};
    long long updates = 0;
    void process_event(const O& o) override {
        state = o;
        ++updates;
    }
};

template <typename O>
struct null_observer : object_observer<O> {
    void process_event(const O&) override {}
};

// ---------------------------------------------------------------------------
// Demo domain: a little market data plant
// ---------------------------------------------------------------------------

struct Tick      { std::int32_t symbol; double price; std::int32_t size; };
struct Trade     { std::int32_t symbol; double price; std::int32_t size; std::int64_t id; };
struct Heartbeat { std::int64_t seq; };

// A 4 KiB payload frame for the raw-bandwidth scenarios. Frames travel by
// const& - the signal moves references, not bytes - and the observers
// checksum every word, so the reported GB/s is data genuinely consumed.
struct Frame {
    std::uint64_t seq;
    std::array<std::uint64_t, 511> words;
};
static_assert(sizeof(Frame) == 4096);

struct frame_sink : object_observer<Frame> {
    std::atomic<std::uint64_t> checksum{0};
    std::atomic<long long> frames{0};
    void process_event(const Frame& f) override {
        std::uint64_t s = f.seq;
        for (const std::uint64_t w : f.words)
            s += w;
        checksum.fetch_add(s, std::memory_order_relaxed);
        frames.fetch_add(1, std::memory_order_relaxed);
    }
};

// One concrete multi-type observer: implements a process_event per type
struct surveillance : multi_observer<Tick, Trade, Heartbeat> {
    long long ticks = 0, trades = 0, beats = 0;
    double notional = 0;

    void process_event(const Tick&) override { ++ticks; }
    void process_event(const Trade& t) override {
        ++trades;
        notional += t.price * t.size;
    }
    void process_event(const Heartbeat&) override { ++beats; }
};

// ---------------------------------------------------------------------------
// Measurement plumbing
// ---------------------------------------------------------------------------

namespace {

long long grand_total_events = 0;
double grand_total_seconds = 0;

double seconds_since(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

void report(const char* name, long long events, double s)
{
    grand_total_events += events;
    grand_total_seconds += s;
    std::printf("  %-46s %10.1f M events/s  %7.1f ns/event\n",
                name, static_cast<double>(events) / s / 1e6,
                s / static_cast<double>(events) * 1e9);
}

void report_gb(const char* name, long long deliveries, double gigabytes, double s)
{
    grand_total_events += deliveries;
    grand_total_seconds += s;
    std::printf("  %-46s %10.1f M events/s  %7.2f GB/s\n",
                name, static_cast<double>(deliveries) / s / 1e6, gigabytes / s);
}

volatile std::uint64_t checksum_sink = 0;   // keeps the payload scans observable

} // namespace

int main(int argc, char** argv)
{
    const long long scale = (argc > 1) ? std::atoll(argv[1]) : 1;
    const long long N = 5'000'000 * (scale > 0 ? scale : 1);

    std::printf("moveable_signal demo - typed publish/subscribe at speed\n");
    std::printf("%lld emissions per scenario (pass a multiplier to scale up)\n\n",
                static_cast<long long>(N));

    // ------------------------------------------------ 1. direct dispatch
    {
        object_emitter<Tick> feed;
        counting_observer<Tick> counter;
        counter.subscribe_to(feed);

        const auto t0 = std::chrono::steady_clock::now();
        for (long long i = 0; i < N; ++i)
            feed.emit(Tick{1, 100.0 + static_cast<double>(i % 100), 10});
        const double s = seconds_since(t0);
        if (counter.count != N)
            return 1;
        report("1 emitter -> 1 observer", N, s);
    }

    // ------------------------------------------------ 2. fan-out x 8
    {
        object_emitter<Tick> feed;
        std::array<counting_observer<Tick>, 8> counters;
        for (auto& c : counters)
            c.subscribe_to(feed);

        const auto t0 = std::chrono::steady_clock::now();
        for (long long i = 0; i < N; ++i)
            feed.emit(Tick{2, 50.0, 1});
        const double s = seconds_since(t0);
        for (const auto& c : counters)
            if (c.count != N)
                return 1;
        report("1 emitter -> 8 observers (deliveries)", N * 8, s);
    }

    // ------------------------------------------------ 3. multi-type plant
    {
        multi_emitter<Tick, Trade, Heartbeat> plant;
        surveillance watcher;                       // subscribes to all three
        watcher.subscribe_all(plant);
        stateful_observer<Tick> last_tick;          // plus a latest-value cache
        last_tick.subscribe_to(plant.as<Tick>());

        const long long rounds = N / 3;
        const auto t0 = std::chrono::steady_clock::now();
        for (long long i = 0; i < rounds; ++i) {
            plant.emit(Tick{3, 101.5, 100});
            plant.emit(Trade{3, 101.5, 100, i});
            plant.emit(Heartbeat{i});
        }
        const double s = seconds_since(t0);
        if (watcher.ticks != rounds || watcher.trades != rounds || watcher.beats != rounds)
            return 1;
        if (last_tick.updates != rounds)
            return 1;
        report("multi_emitter<Tick,Trade,Heartbeat> (deliveries)", rounds * 4, s);
    }

    // ------------------------------------------------ 4. cross-thread firehose
    {
        object_emitter<Heartbeat> bus;
        concurrent_counting_observer<Heartbeat> sink;
        sink.subscribe_to(bus);

        static constexpr int n_threads = 4;
        const long long per_thread = N / n_threads;
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<std::thread> threads;
        for (int t = 0; t < n_threads; ++t)
            threads.emplace_back([&bus, per_thread] {
                for (long long i = 0; i < per_thread; ++i)
                    bus.emit(Heartbeat{i});
            });
        for (auto& t : threads)
            t.join();
        const double s = seconds_since(t0);
        if (sink.count.load() != per_thread * n_threads)
            return 1;
        report("4 threads emitting into 1 observer", per_thread * n_threads, s);
    }

    // ------------------------------------------------ 5. many-to-many mesh, small messages
    // 4 emitters, each driven by its own producer thread; 4 observers, each
    // subscribed to all 4 emitters. 16 routes; every emission delivers 4 times.
    {
        static constexpr int n_emitters = 4;
        static constexpr int n_observers = 4;
        std::array<object_emitter<Tick>, n_emitters> emitters;
        std::array<concurrent_counting_observer<Tick>, n_observers> observers;
        for (auto& o : observers)
            for (auto& e : emitters)
                o.subscribe_to(e);

        const long long per_thread = N / n_emitters;
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<std::thread> threads;
        for (int t = 0; t < n_emitters; ++t)
            threads.emplace_back([&emitters, t, per_thread] {
                for (long long i = 0; i < per_thread; ++i)
                    emitters[static_cast<std::size_t>(t)].emit(Tick{t, 1.0, 1});
            });
        for (auto& t : threads)
            t.join();
        const double s = seconds_since(t0);
        for (const auto& o : observers)
            if (o.count.load() != per_thread * n_emitters)
                return 1;
        report("many-to-many mesh 4x4 (deliveries)", per_thread * n_emitters * n_observers, s);
    }

    // ------------------------------------------------ 6. raw bandwidth, single route
    // 4 KiB frames by const& - the observer checksums every word, so this is
    // bytes genuinely consumed, not pointers waved at
    {
        object_emitter<Frame> feed;
        frame_sink sink;
        sink.subscribe_to(feed);

        Frame f{};
        f.seq = 1;
        for (std::size_t i = 0; i < f.words.size(); ++i)
            f.words[i] = i;

        const long long n = N / 10;
        const auto t0 = std::chrono::steady_clock::now();
        for (long long i = 0; i < n; ++i) {
            f.seq = static_cast<std::uint64_t>(i);
            feed.emit(f);
        }
        const double s = seconds_since(t0);
        if (sink.frames.load() != n)
            return 1;
        checksum_sink += sink.checksum.load();
        report_gb("4 KiB frames -> 1 observer (checksummed)", n,
                  static_cast<double>(n) * sizeof(Frame) / 1e9, s);
    }

    // ------------------------------------------------ 7. many-to-many mesh, raw bandwidth
    // The full mesh moving bulk data: 4 producer threads, 4 frame sinks each
    // consuming every producer's frames. Every emission is scanned 4 times.
    {
        static constexpr int n_emitters = 4;
        static constexpr int n_observers = 4;
        std::array<object_emitter<Frame>, n_emitters> emitters;
        std::array<frame_sink, n_observers> sinks;
        for (auto& o : sinks)
            for (auto& e : emitters)
                o.subscribe_to(e);

        const long long per_thread = N / 50;
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<std::thread> threads;
        for (int t = 0; t < n_emitters; ++t)
            threads.emplace_back([&emitters, t, per_thread] {
                Frame f{};
                for (std::size_t i = 0; i < f.words.size(); ++i)
                    f.words[i] = static_cast<std::uint64_t>(t) + i;
                for (long long i = 0; i < per_thread; ++i) {
                    f.seq = static_cast<std::uint64_t>(i);
                    emitters[static_cast<std::size_t>(t)].emit(f);
                }
            });
        for (auto& t : threads)
            t.join();
        const double s = seconds_since(t0);
        long long delivered = 0;
        for (auto& o : sinks) {
            delivered += o.frames.load();
            checksum_sink += o.checksum.load();
        }
        if (delivered != per_thread * n_emitters * n_observers)
            return 1;
        report_gb("many-to-many mesh 4x4, 4 KiB frames", delivered,
                  static_cast<double>(delivered) * sizeof(Frame) / 1e9, s);
    }

    // ------------------------------------------------ 8. emitters move; connections survive
    {
        object_emitter<Tick> feed;
        counting_observer<Tick> counter;
        counter.subscribe_to(feed);
        feed.emit(Tick{4, 1.0, 1});

        std::vector<object_emitter<Tick>> plant;    // the rule-of-zero story:
        plant.push_back(std::move(feed));           // wire first, place anywhere after
        plant.reserve(64);                          // even a reallocation move
        plant.front().emit(Tick{4, 2.0, 1});

        if (counter.count != 2 || !counter.subscribed())
            return 1;
        std::printf("  %-46s %s\n", "emitter moved into a vector (twice)",
                    "connections survived - counts intact");
    }

    // ------------------------------------------------ the punchline
    const double sustained = static_cast<double>(grand_total_events) / grand_total_seconds;
    std::printf("\n  delivered %.0fM events in %.2fs - sustained %.1fM events/s\n",
                static_cast<double>(grand_total_events) / 1e6, grand_total_seconds,
                sustained / 1e6);
    std::printf("  at this rate: %.1f billion events per hour, on this machine\n",
                sustained * 3600.0 / 1e9);
    std::printf("\n  memory: sizeof(moveable_signal) = %zu bytes (one shared_ptr),\n"
                "  sizeof(connection) = %zu bytes; connect allocates, emit never does -\n"
                "  every number above ran in constant memory\n",
                sizeof(snicholls::moveable_signal<const Tick&>),
                sizeof(snicholls::connection));
    std::printf("\n  the GB/s figures are bytes checksummed by consumers. Payloads travel\n"
                "  by const& - zero copies in the mesh - so hot data stays cache-hot,\n"
                "  which is why delivery bandwidth can exceed DRAM bandwidth\n");
    return 0;
}
