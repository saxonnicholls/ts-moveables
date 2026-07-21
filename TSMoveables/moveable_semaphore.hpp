//
//  moveable_semaphore.hpp
//  TSMoveables
//
//  Created by Saxon Nicholls on 21/7/2026.
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables
//

#ifndef moveable_semaphore_hpp
#define moveable_semaphore_hpp

#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstddef>
#include <stdexcept>

namespace snicholls
{
    // Moveable counting semaphore
    //
    // std::counting_semaphore is immovable and its count cannot be observed, so
    // a wrapper could never preserve its state across a move. Instead this is a
    // small, portable semaphore built on mutex + condition_variable whose entire
    // state lives under the mutex - which makes the move guarantee exact rather
    // than best-effort: a move transfers the permit count if and only if no
    // thread is blocked in acquire on either side; otherwise it throws
    // std::runtime_error. The moved-from semaphore is left valid with a count
    // of zero (permits are a resource - they transfer, they do not duplicate).
    struct moveable_semaphore {

    private:
        mutable std::mutex m;
        std::condition_variable cv;
        std::ptrdiff_t count{0};
        int waiters{0};

        static void ensure_no_waiters(int waiters_) {
            if (waiters_ != 0)
                throw std::runtime_error("moveable_semaphore: moving while threads are waiting");
        }

    public:
        explicit moveable_semaphore(std::ptrdiff_t initial = 0) : count(initial) {}

        moveable_semaphore(const moveable_semaphore&) = delete;
        moveable_semaphore& operator=(const moveable_semaphore&) = delete;

        moveable_semaphore(moveable_semaphore&& other) {
            std::lock_guard<std::mutex> lock(other.m);
            ensure_no_waiters(other.waiters);
            count = other.count;
            other.count = 0;
        }

        moveable_semaphore& operator=(moveable_semaphore&& other) {
            if (this != &other) {
                std::scoped_lock lock(m, other.m);
                ensure_no_waiters(waiters);
                ensure_no_waiters(other.waiters);
                count = other.count;
                other.count = 0;
            }
            return *this;
        }

        void release(std::ptrdiff_t update = 1) {
            {
                std::lock_guard<std::mutex> lock(m);
                count += update;
            }
            if (update == 1)
                cv.notify_one();
            else
                cv.notify_all();
        }

        void acquire() {
            std::unique_lock<std::mutex> lock(m);
            ++waiters;
            cv.wait(lock, [this] { return count > 0; });
            --waiters;
            --count;
        }

        bool try_acquire() {
            std::lock_guard<std::mutex> lock(m);
            if (count > 0) {
                --count;
                return true;
            }
            return false;
        }

        template <typename Rep, typename Period>
        bool try_acquire_for(const std::chrono::duration<Rep, Period>& d) {
            std::unique_lock<std::mutex> lock(m);
            ++waiters;
            const bool ok = cv.wait_for(lock, d, [this] { return count > 0; });
            --waiters;
            if (ok)
                --count;
            return ok;
        }

        template <typename Clock, typename Duration>
        bool try_acquire_until(const std::chrono::time_point<Clock, Duration>& t) {
            std::unique_lock<std::mutex> lock(m);
            ++waiters;
            const bool ok = cv.wait_until(lock, t, [this] { return count > 0; });
            --waiters;
            if (ok)
                --count;
            return ok;
        }

        // Current permit count - a snapshot, for testing and diagnostics
        std::ptrdiff_t available() const {
            std::lock_guard<std::mutex> lock(m);
            return count;
        }
    };

    // Type Aliases
    using moveable_counting_semaphore = moveable_semaphore;

} // namespace snicholls

#endif /* moveable_semaphore_hpp */
