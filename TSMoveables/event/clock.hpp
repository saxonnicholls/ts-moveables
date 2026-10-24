//
//  event/clock.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - the loop's sense of time, made replaceable
//
//      event_loop loop;                                  // real time, as always
//
//      auto clk = std::make_shared<manual_clock>();       // time moves on command
//      loop.set_clock(clk);
//      auto t = loop.timer(100ms, [] { fired = true; });
//      clk->advance(99ms);   // nothing
//      clk->advance(1ms);    // fires, with no wall time spent at all
//
//      auto fast = std::make_shared<scaled_clock>(100.0); // replay at 100x
//      loop.set_clock(fast);
//      fast->set_speed(0.5);                              // slow down, mid-run
//      fast->pause();                                     // and hold
//
//  ------------------------------------------------------------------- why
//
//  Timers were nailed to steady_clock::now(), which is exactly right for the
//  one case it serves and useless for every other. A trading engine has to run
//  the same strategy timers live, in replay at 0.01x or 100x, paused, and
//  single-stepped - and with the clock hard-wired it could only do the first,
//  so the consumer that reported this carries its own clock, its own timer heap
//  and its own pacer in parallel with the loop's. That is three pieces of
//  duplicated machinery to avoid one `now()`.
//
//  A test pays the same tax in a smaller currency: "does this fire after 30
//  seconds" costs thirty seconds, so it gets written as "after 30 milliseconds"
//  and tests something subtly different, or gets skipped.
//
//  ------------------------------------------------------ what a clock must do
//
//  Two questions, and the second is the one that is easy to miss.
//
//    now()        what time is it, in the loop's timeline.
//
//    block_for()  how long may the poller sleep IN REAL TIME, waiting for a
//                 deadline expressed in the loop's timeline. These are not the
//                 same number as soon as the two timelines differ: at 100x a
//                 virtual second is 10 real milliseconds, and a manual clock
//                 must not be waited for at all, because nothing but an
//                 advance() will ever make its deadline arrive.
//
//  Without the second one, a virtual clock either spins hot or sleeps through
//  its own timers. It is the reason this is an interface rather than just a
//  now() callback.
//
//  Ties still break on the loop's own sequence number, so two timers due at the
//  same instant fire in the order they were created, whatever the clock.
//
//  ---------------------------------------------------------------- the cost
//
//  None, unless used. The loop holds a null clock pointer by default and calls
//  steady_clock::now() directly - one predictable branch, no virtual call, on a
//  path that already performs a syscall-ish read. set_clock() is what opts in.
//

#ifndef event_clock_hpp
#define event_clock_hpp

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <utility>

namespace snicholls {

// The loop's clock. time_point keeps steady_clock's representation so the
// timer heap and every public signature are unchanged - what varies is who
// decides what the number means.
class loop_clock {
public:
    using time_point = std::chrono::steady_clock::time_point;
    using duration   = std::chrono::nanoseconds;

    virtual ~loop_clock() = default;

    virtual time_point now() const noexcept = 0;

    // Real milliseconds the poller may block while the next deadline sits at
    // `until` in this clock's timeline. -1 means "block indefinitely", 0 means
    // "do not block".
    virtual long long block_for(time_point until) const noexcept = 0;

    // The loop installs a waker here, so a clock that only moves on command can
    // say so. Without it, advance() would leave the loop asleep in the poller
    // until something unrelated woke it - the timer would be due and nobody
    // would look.
    virtual void set_waker(std::function<void()> wake) { waker_ = std::move(wake); }

protected:
    void wake() const
    {
        if (waker_)
            waker_();
    }

private:
    std::function<void()> waker_;
};

// --------------------------------------------------------------- real time
//
// What the loop does with no clock set. Provided so a caller can be explicit,
// and so a scaled clock has something to anchor against.
class steady_clock_source final : public loop_clock {
public:
    time_point now() const noexcept override { return std::chrono::steady_clock::now(); }

    long long block_for(time_point until) const noexcept override
    {
        const auto ms = std::chrono::ceil<std::chrono::milliseconds>(until - now()).count();
        return ms < 0 ? 0 : ms;
    }
};

// ------------------------------------------------------------ manual time
//
// Time moves only when told. The whole point is that a deadline arrives for
// free: a test for a one-hour timeout costs no wall clock, and single-stepping
// is just advance() in a loop.
//
// block_for always says "do not block": the loop must come back and look,
// because an advance() from another thread is the only thing that will ever
// make a deadline due, and advance() also wakes the loop so it does not spin
// while idle.
class manual_clock final : public loop_clock {
public:
    explicit manual_clock(time_point start = time_point{})
        : t_(start.time_since_epoch().count()) {}

    time_point now() const noexcept override
    {
        return time_point(duration(t_.load(std::memory_order_acquire)));
    }

    // Safe from any thread, which is what lets a test drive a loop running on
    // another one.
    void advance(duration d)
    {
        if (d.count() < 0)
            return;                     // time does not go backwards here
        t_.fetch_add(d.count(), std::memory_order_acq_rel);
        wake();
    }

    void advance_to(time_point t)
    {
        const auto target = t.time_since_epoch().count();
        auto cur = t_.load(std::memory_order_acquire);
        while (target > cur &&
               !t_.compare_exchange_weak(cur, target, std::memory_order_acq_rel))
            ;                           // monotonic: only ever forwards
        wake();
    }

    long long block_for(time_point) const noexcept override { return 0; }

private:
    std::atomic<duration::rep> t_;
};

// ------------------------------------------------------------ scaled time
//
// Real time, multiplied. speed > 1 replays faster than life, < 1 slower, 0 is
// paused.
//
// Anchored rather than computed from a start point: now() is
// virtual_anchor + (real_now - real_anchor) * speed, and changing the speed
// RE-ANCHORS both ends first. Without that, a speed change would retroactively
// rewrite all the time that had already passed - a run switched from 1x to 2x
// an hour in would jump an hour into the future, which is not what anybody
// means by "speed up".
class scaled_clock final : public loop_clock {
public:
    explicit scaled_clock(double speed = 1.0, time_point start = time_point{})
        : virt_(start), real_(std::chrono::steady_clock::now()), speed_(speed <= 0 ? 0.0 : speed)
    {
    }

    time_point now() const noexcept override
    {
        std::lock_guard<std::mutex> g(m_);
        return now_locked();
    }

    long long block_for(time_point until) const noexcept override
    {
        std::lock_guard<std::mutex> g(m_);
        if (speed_ == 0.0)
            return -1;                  // paused: that deadline is never coming
        const auto virt_left = until - now_locked();
        if (virt_left.count() <= 0)
            return 0;
        // Real time is virtual time divided by the rate it is running at.
        const double real_ns = double(virt_left.count()) / speed_;
        const double real_ms = real_ns / 1e6;
        if (real_ms <= 0.0)
            return 0;
        return real_ms > 1e9 ? static_cast<long long>(1e9)
                             : static_cast<long long>(real_ms) + 1;   // round up
    }

    // Re-anchors, so the change applies from here rather than rewriting history.
    void set_speed(double s)
    {
        {
            std::lock_guard<std::mutex> g(m_);
            virt_ = now_locked();
            real_ = std::chrono::steady_clock::now();
            speed_ = s <= 0 ? 0.0 : s;
        }
        wake();                         // the next deadline just moved
    }

    double speed() const
    {
        std::lock_guard<std::mutex> g(m_);
        return speed_;
    }

    void pause()
    {
        std::lock_guard<std::mutex> g(m_);
        if (speed_ == 0.0)
            return;
        virt_ = now_locked();
        real_ = std::chrono::steady_clock::now();
        resume_speed_ = speed_;
        speed_ = 0.0;
        // No wake: nothing can become due while paused.
    }

    void resume()
    {
        {
            std::lock_guard<std::mutex> g(m_);
            if (speed_ != 0.0)
                return;
            virt_ = now_locked();
            real_ = std::chrono::steady_clock::now();
            speed_ = resume_speed_;
        }
        wake();
    }

    bool paused() const
    {
        std::lock_guard<std::mutex> g(m_);
        return speed_ == 0.0;
    }

private:
    time_point now_locked() const noexcept
    {
        if (speed_ == 0.0)
            return virt_;
        const auto real_elapsed = std::chrono::steady_clock::now() - real_;
        const auto scaled = duration(
            static_cast<duration::rep>(double(
                std::chrono::duration_cast<duration>(real_elapsed).count()) * speed_));
        return virt_ + scaled;
    }

    mutable std::mutex m_;
    time_point         virt_;
    time_point         real_;
    double             speed_;
    double             resume_speed_ = 1.0;
};

} // namespace snicholls

#endif /* event_clock_hpp */
