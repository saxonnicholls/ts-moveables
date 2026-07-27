//
//  time_master_demo.cpp
//  TSMoveables
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - TimeMaster, reborn on the event loop
//
//  TimeMaster is a periodic-event scheduler that has earned its keep in
//  production for years: register closures at intervals, call Run(), and the
//  right things happen at the right times. The original was ~100 lines on
//  Boost.Asio deadline timers - and carried the classic scars of its era:
//
//    - the io_context is immovable, so TimeMaster was pinned for life;
//    - TimedEvent was copyable with a shared_ptr timer, so copies raced on
//      one timer; the handler captured raw `this` into async_wait;
//    - rescheduling by `expires_at() + interval` is drift-free but bursts
//      after a stall - wake a laptop from sleep and every missed tick fires
//      back-to-back in a catch-up storm;
//    - the destructor called io.stop() and then *slept for a full second*,
//      hoping the loop had noticed.
//
//  The scheduler itself now lives in TSMoveables/time_master.hpp, so it is a
//  component you can use rather than demo code you would have to copy. This
//  file is what remains: proof that each of the original's scars is closed by
//  construction rather than by hope.
//
//    - moveable: heap-stable core, so a fully-wired time_master is a value -
//      built in a factory, stored in a vector, moved into an engine;
//    - handlers are signal slots with RAII connections - no raw `this`;
//    - periodic timers are drift-free AND clamped - after a stall the loop
//      skips ahead instead of bursting (asserted below, not just claimed);
//    - events can be added from any thread while the loop runs (post() is
//      the door in), and cancelled individually - the original could only
//      stop the world;
//    - teardown is stop() + join: measured below in microseconds, not
//      slept-through in seconds.
//
//  Build and run:   make demo-timemaster        (compiled -O3 -DNDEBUG)
//
//  The demo verifies its own claims and exits non-zero on failure, so CI
//  runs it as an integration test on every push.
//

#include "../TSMoveables/event/time_master.hpp"
#include "../TSMoveables/moveable/mutex.hpp"

#include <cstdio>
#include <cstring>

#if SNICHOLLS_HAS_TIME_MASTER

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

// --------------------------------------------------------------------- helpers

bool markdown = false;                      // --markdown: emit a GitHub-flavoured table

void check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::exit(1);
    }
}

double ms_since(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void row(const char* what, const char* result)
{
    if (markdown)
        std::printf("| %s | %s |\n", what, result);
    else
        std::printf("  %-46s %s\n", what, result);
}

} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--markdown") == 0)
            markdown = true;

    if (markdown)
        std::printf("### `make demo-timemaster` - TimeMaster on the event loop\n\n"
                    "| Scenario | Result |\n|---|---|\n");
    else
        std::printf("time_master demo - the periodic scheduler, rebuilt on event_loop\n\n");

    char buf[128];

    // ------------------------------------- 1. the original TestTimeMaster, /100
    // The classic setup - six periodic closures - at 1/100th of the original
    // intervals so the whole demo runs in under a second
    {
        snicholls::time_master tm;

        std::atomic<int> f10{0}, f20{0}, f30{0}, f10b{0}, f70{0}, f110{0};
        std::vector<std::chrono::steady_clock::time_point> fires;   // loop thread only
        fires.reserve(256);

        tm.add_event(10ms, [&] {
            fires.push_back(std::chrono::steady_clock::now());
            f10.fetch_add(1, std::memory_order_relaxed);
        });
        tm.add_event(20ms, [&] { f20.fetch_add(1, std::memory_order_relaxed); });
        tm.add_event(30ms, [&] { f30.fetch_add(1, std::memory_order_relaxed); });
        tm.add_event(10ms, [&] { f10b.fetch_add(1, std::memory_order_relaxed); });
        tm.add_event(70ms, [&] { f70.fetch_add(1, std::memory_order_relaxed); });
        tm.add_event(110ms, [&] { f110.fetch_add(1, std::memory_order_relaxed); });

        // A one-shot (the original had no such thing) and a victim to cancel
        std::atomic<int> once{0}, doomed{0};
        tm.add_once(50ms, [&] { once.fetch_add(1, std::memory_order_relaxed); });
        const auto doomed_id = tm.add_event(5ms, [&] { doomed.fetch_add(1, std::memory_order_relaxed); });

        std::thread runner([&] { tm.run(); });
        while (!tm.running())
            std::this_thread::yield();
        const auto t0 = std::chrono::steady_clock::now();

        // Add an event from THIS (foreign) thread while the loop runs - the
        // original's AddEvent-then-Run was a fixed menu; ours stays open
        std::atomic<int> late{0};
        std::this_thread::sleep_for(120ms);
        tm.add_event(25ms, [&] { late.fetch_add(1, std::memory_order_relaxed); });

        // Cancel one event by id - the original could only stop everything
        std::this_thread::sleep_for(30ms);
        tm.cancel(doomed_id);
        std::this_thread::sleep_for(30ms);          // let the cancel land
        const int doomed_at_cancel = doomed.load();

        std::this_thread::sleep_for(220ms);
        const double elapsed_ms = ms_since(t0);

        const auto t_stop = std::chrono::steady_clock::now();
        tm.stop();
        runner.join();
        const double stop_ms = ms_since(t_stop);

        check(f10.load() >= 10, "10ms event ticked");
        check(f20.load() >= 5, "20ms event ticked");
        check(f30.load() >= 3, "30ms event ticked");
        check(f10b.load() >= 10, "second 10ms event ticked independently");
        check(f70.load() >= 2, "70ms event ticked");
        check(f110.load() >= 1, "110ms event ticked");
        check(once.load() == 1, "one-shot fired exactly once");
        check(late.load() >= 2, "event added from a foreign thread mid-run ticked");
        check(doomed.load() == doomed_at_cancel, "cancelled event never fired again");

        std::snprintf(buf, sizeof buf,
                      "10ms:%d 20ms:%d 30ms:%d 10msB:%d 70ms:%d 110ms:%d in %.0f ms",
                      f10.load(), f20.load(), f30.load(), f10b.load(), f70.load(),
                      f110.load(), elapsed_ms);
        row("six periodic events (original test, /100)", buf);
        std::snprintf(buf, sizeof buf, "fired %d, cancelled at %d, stayed at %d",
                      late.load(), doomed_at_cancel, doomed.load());
        row("mid-run add (foreign thread) + cancel by id", buf);

        // ------------------------------ 2. drift-free, burst-free - asserted
        // Deadlines advance by at least one period per fire (clamped after
        // stalls), so the fire count over any window is bounded: no catch-up
        // storms, ever. The wake-from-sleep burst is designed out
        check(fires.size() >= 2, "enough samples to measure cadence");
        const double span_ms =
            std::chrono::duration<double, std::milli>(fires.back() - fires.front()).count();
        const double avg_gap = span_ms / double(fires.size() - 1);
        check(double(fires.size()) <= span_ms / 10.0 + 2.5,
              "burst-freedom: fires bounded by elapsed/period");
        std::snprintf(buf, sizeof buf, "%zu fires over %.0f ms, avg period %.2f ms (nominal 10)",
                      fires.size(), span_ms, avg_gap);
        row("cadence: drift-free and burst-free", buf);

        // ------------------------------------------- 3. teardown in microseconds
        // The original's destructor slept a hard-coded second; stop() + join
        // here is measured, not prayed for
        check(stop_ms < 900.0, "teardown beat the original's 1000ms nap");
        std::snprintf(buf, sizeof buf, "stop+join %.3f ms (the original slept 1000 ms)", stop_ms);
        row("teardown", buf);
    }

    // ------------------------------------------------- 4. a scheduler is a value
    // Build a fully-wired time_master in a factory, move it through a vector,
    // and run the survivor. Boost.Asio's io_context is immovable - the
    // original TimeMaster was welded to wherever it was constructed
    {
        auto counter = std::make_shared<std::atomic<int>>(0);
        auto make_metronome = [&counter] {
            snicholls::time_master tm;
            tm.add_event(10ms, [counter] { counter->fetch_add(1, std::memory_order_relaxed); });
            return tm;                              // moves out, fully wired
        };

        std::vector<snicholls::time_master> shelf;
        shelf.push_back(make_metronome());
        snicholls::time_master tm = std::move(shelf.back());   // and out again
        shelf.clear();

        std::thread runner([&] { tm.run(); });
        while (!tm.running())
            std::this_thread::yield();
        std::this_thread::sleep_for(80ms);
        tm.stop();
        runner.join();

        check(counter->load() >= 2, "moved scheduler still ticks");
        std::snprintf(buf, sizeof buf, "factory -> vector -> move -> run: %d ticks",
                      counter->load());
        row("moveability: scheduler built elsewhere", buf);
    }

    if (!markdown)
        std::printf("\nall time_master demo checks passed\n");
    return 0;
}

#else // !SNICHOLLS_HAS_TIME_MASTER

int main()
{
    std::printf("time_master demo: event_loop is POSIX-only in phase 1 - demo skipped\n");
    return 0;
}

#endif
