//
//  time_master.hpp
//  TSMoveables
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - a periodic event scheduler on the event loop
//
//  The Legendary TimeMaster - work horse for decades of trading 
// 
//  TimeMaster is a scheduler that has earned its keep in production for years:
//  register closures at intervals, call run(), and the right things happen at
//  the right times. The original was built on Boost.Asio deadline timers and
//  carried the scars of its era - this is the same tool on snicholls::
//  event_loop, with each scar closed by construction:
//
//    - **Moveable.** The core is heap-stable, so a fully wired scheduler is a
//      value: build it in a factory, park it in a vector, move it into an
//      engine. Asio's io_context is immovable, so its ancestor was welded to
//      wherever it was constructed.
//    - **No raw `this` in a handler.** Events are signal slots with RAII
//      connections; cancelling one cannot leave a dangling callback behind.
//    - **Drift-free and burst-free.** Periodic events reschedule from the
//      previous deadline, so they do not drift - but clamp to now after a
//      stall, so waking a laptop from sleep produces one tick rather than
//      four hundred queued catch-up ticks.
//    - **Open while running.** Events can be added and cancelled from any
//      thread while the loop runs; the loop's own thread contract is honoured
//      by marshalling through post(). The original could only stop the world.
//    - **Teardown is immediate.** stop() returns at once. The original's
//      destructor called io.stop() and then slept for a hard-coded second,
//      hoping the loop had noticed.
//
//      snicholls::time_master tm;
//      tm.add_event(1000ms, [] { heartbeat(); });      // periodic
//      tm.add_once(5s,      [] { warm_up_done(); });   // one-shot
//      std::thread t([&] { tm.run(); });               // or run() inline
//      ...
//      tm.stop();
//
//  POSIX only, since it follows the event loop; on Windows this header
//  compiles to nothing and SNICHOLLS_HAS_TIME_MASTER is 0.
//

#ifndef time_master_hpp
#define time_master_hpp

#include "event_loop.hpp"

#if !SNICHOLLS_HAS_EVENT_LOOP
#define SNICHOLLS_HAS_TIME_MASTER 0
#else
#define SNICHOLLS_HAS_TIME_MASTER 1

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace snicholls {

class time_master {
public:
    using closure = std::function<void()>;
    using event_id = std::uint64_t;

    time_master() = default;
    time_master(time_master&&) noexcept = default;
    time_master& operator=(time_master&&) noexcept = default;
    time_master(const time_master&) = delete;
    time_master& operator=(const time_master&) = delete;

    ~time_master()
    {
        if (c_)
            c_->loop.stop();                    // returns immediately; no nap
    }

    // Repeat `fn` every `interval`. Safe to call before or during run(), from
    // any thread. Returns an id for cancel().
    template <typename Rep, typename Period>
    event_id add_event(std::chrono::duration<Rep, Period> interval, closure fn)
    {
        return add(std::chrono::duration_cast<std::chrono::nanoseconds>(interval),
                   true, std::move(fn));
    }

    // Run `fn` once, `delay` from now.
    template <typename Rep, typename Period>
    event_id add_once(std::chrono::duration<Rep, Period> delay, closure fn)
    {
        return add(std::chrono::duration_cast<std::chrono::nanoseconds>(delay),
                   false, std::move(fn));
    }

    // Cancel one event by id, leaving the others alone. Safe from any thread.
    void cancel(event_id id)
    {
        auto c = c_;
        on_loop([c, id] {
            for (auto& e : c->entries)
                if (e.id == id) {
                    e.timer.cancel();
                    e.connection.disconnect();
                }
        });
    }

    // Cancel everything, without stopping the loop.
    void clear()
    {
        auto c = c_;
        on_loop([c] {
            for (auto& e : c->entries) {
                e.timer.cancel();
                e.connection.disconnect();
            }
            c->entries.clear();
        });
    }

    void run() { c_->loop.run(); }               // blocks until stop()
    bool run_once(std::chrono::milliseconds max_wait = std::chrono::milliseconds{0})
    {
        return c_->loop.run_once(max_wait);
    }
    void stop() { c_->loop.stop(); }             // safe from any thread
    bool running() const noexcept { return c_->loop.running(); }

    // The loop underneath, for adding watches and timers of your own
    event_loop& loop() noexcept { return c_->loop; }

    std::size_t event_count() const noexcept { return c_->entries.size(); }

private:
    struct entry {
        event_id id{};
        event_loop::timer timer;
        scoped_connection connection;
    };

    struct core {
        event_loop loop;
        std::vector<entry> entries;              // pre-run, or loop thread only
        std::atomic<event_id> next_id{1};
    };

    // The loop enforces its thread contract loudly once running, so mutation
    // marshals through post(); before run(), setup is direct
    template <typename F>
    void on_loop(F&& f)
    {
        if (c_->loop.running())
            c_->loop.post(std::forward<F>(f));
        else
            f();
    }

    event_id add(std::chrono::nanoseconds interval, bool periodic, closure fn)
    {
        const event_id id = c_->next_id.fetch_add(1, std::memory_order_relaxed);
        auto c = c_;
        on_loop([c, id, interval, periodic, fn = std::move(fn)]() mutable {
            auto t = periodic ? c->loop.every(interval) : c->loop.after(interval);
            scoped_connection sc{t.on_fire().connect(std::move(fn))};
            c->entries.push_back(entry{id, std::move(t), std::move(sc)});
        });
        return id;
    }

    std::shared_ptr<core> c_ = std::make_shared<core>();
};

} // namespace snicholls

#endif // SNICHOLLS_HAS_EVENT_LOOP
#endif /* time_master_hpp */
