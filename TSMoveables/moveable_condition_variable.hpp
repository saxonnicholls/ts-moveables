//
//  moveable_condition_variable.hpp
//  TSMoveables
//
//  Created by Saxon Nicholls on 21/7/2026.
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables
//

#ifndef moveable_condition_variable_hpp
#define moveable_condition_variable_hpp

#include <condition_variable>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace snicholls
{
    // Moveable condition variable
    // Prefer composition over inheritance
    //
    // A condition variable with no waiters carries no observable state, so
    // "moving" one means: verify nobody is blocked on the source (or the
    // destination, on assignment) and start fresh. We track waiters with an
    // atomic counter maintained by every wait call; a move while the counter is
    // non-zero throws std::runtime_error rather than stranding blocked threads.
    //
    // As with moveable_mutex, the check makes misuse loud rather than making
    // concurrent move + wait a sensible program - moves should happen when the
    // object is quiescent (e.g. moving a container of parked workers around).
    //
    // Works with std::condition_variable (waits require
    // std::unique_lock<std::mutex>) and std::condition_variable_any (waits work
    // with any lockable, including moveable_mutex).
    template <typename CV = std::condition_variable>
    struct moveable_condition_variable {

        using condition_variable_type = CV;

    private:
        CV cv;
        std::atomic<int> waiters{0};

        struct waiter_guard {
            std::atomic<int>& w;
            explicit waiter_guard(std::atomic<int>& waiters_) noexcept : w(waiters_) { ++w; }
            ~waiter_guard() { --w; }
        };

        static void ensure_quiescent(const std::atomic<int>& waiters_) {
            if (waiters_.load() != 0)
                throw std::runtime_error("moveable_condition_variable: moving while threads are waiting");
        }

    public:
        moveable_condition_variable() = default;

        moveable_condition_variable(const moveable_condition_variable&) = delete;
        moveable_condition_variable& operator=(const moveable_condition_variable&) = delete;

        moveable_condition_variable(moveable_condition_variable&& other) : cv() {
            ensure_quiescent(other.waiters);
        }

        moveable_condition_variable& operator=(moveable_condition_variable&& other) {
            if (this != &other) {
                ensure_quiescent(waiters);
                ensure_quiescent(other.waiters);
            }
            return *this;
        }

        ~moveable_condition_variable() = default;

        void notify_one() noexcept { cv.notify_one(); }
        void notify_all() noexcept { cv.notify_all(); }

        template <typename Lock>
        void wait(Lock& lock) {
            waiter_guard g(waiters);
            cv.wait(lock);
        }

        template <typename Lock, typename Predicate>
        void wait(Lock& lock, Predicate pred) {
            waiter_guard g(waiters);
            cv.wait(lock, std::move(pred));
        }

        template <typename Lock, typename Rep, typename Period>
        std::cv_status wait_for(Lock& lock, const std::chrono::duration<Rep, Period>& d) {
            waiter_guard g(waiters);
            return cv.wait_for(lock, d);
        }

        template <typename Lock, typename Rep, typename Period, typename Predicate>
        bool wait_for(Lock& lock, const std::chrono::duration<Rep, Period>& d, Predicate pred) {
            waiter_guard g(waiters);
            return cv.wait_for(lock, d, std::move(pred));
        }

        template <typename Lock, typename Clock, typename Duration>
        std::cv_status wait_until(Lock& lock, const std::chrono::time_point<Clock, Duration>& t) {
            waiter_guard g(waiters);
            return cv.wait_until(lock, t);
        }

        template <typename Lock, typename Clock, typename Duration, typename Predicate>
        bool wait_until(Lock& lock, const std::chrono::time_point<Clock, Duration>& t, Predicate pred) {
            waiter_guard g(waiters);
            return cv.wait_until(lock, t, std::move(pred));
        }

        // Number of threads currently blocked in a wait on this object
        int waiting() const noexcept { return waiters.load(); }

        CV& native() noexcept { return cv; }
        operator CV&() noexcept { return cv; }
    };

    // Type Aliases
    using moveable_condition_variable_any = moveable_condition_variable< std::condition_variable_any >;

} // namespace snicholls

#endif /* moveable_condition_variable_hpp */
