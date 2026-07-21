//
//  moveable_latch.hpp
//  TSMoveables
//
//  Created by Saxon Nicholls on 21/7/2026.
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables
//

#ifndef moveable_latch_hpp
#define moveable_latch_hpp

#include <mutex>
#include <condition_variable>
#include <cstddef>
#include <stdexcept>

namespace snicholls
{
    // Moveable latch
    //
    // std::latch is immovable and its remaining count cannot be observed, so a
    // wrapper could never preserve its state across a move. This is a portable
    // single-use latch built on mutex + condition_variable whose entire state
    // lives under the mutex, making the move guarantee exact: a move transfers
    // the remaining count if and only if no thread is blocked in wait on either
    // side; otherwise it throws std::runtime_error. The moved-from latch is
    // left released (count zero).
    //
    // Unlike std::latch, counting down past zero throws std::logic_error rather
    // than being undefined behaviour.
    struct moveable_latch {

    private:
        mutable std::mutex m;
        mutable std::condition_variable cv;
        std::ptrdiff_t count{0};
        mutable int waiters{0};

        static void ensure_no_waiters(int waiters_) {
            if (waiters_ != 0)
                throw std::runtime_error("moveable_latch: moving while threads are waiting");
        }

    public:
        explicit moveable_latch(std::ptrdiff_t expected = 0) : count(expected) {}

        moveable_latch(const moveable_latch&) = delete;
        moveable_latch& operator=(const moveable_latch&) = delete;

        moveable_latch(moveable_latch&& other) {
            std::lock_guard<std::mutex> lock(other.m);
            ensure_no_waiters(other.waiters);
            count = other.count;
            other.count = 0;
        }

        moveable_latch& operator=(moveable_latch&& other) {
            if (this != &other) {
                std::scoped_lock lock(m, other.m);
                ensure_no_waiters(waiters);
                ensure_no_waiters(other.waiters);
                count = other.count;
                other.count = 0;
            }
            return *this;
        }

        void count_down(std::ptrdiff_t n = 1) {
            bool released = false;
            {
                std::lock_guard<std::mutex> lock(m);
                if (n > count)
                    throw std::logic_error("moveable_latch: count_down below zero");
                count -= n;
                released = (count == 0);
            }
            if (released)
                cv.notify_all();
        }

        // Not noexcept, unlike std::latch::try_wait: ours takes the mutex,
        // and locking may throw std::system_error
        bool try_wait() const {
            std::lock_guard<std::mutex> lock(m);
            return count == 0;
        }

        void wait() const {
            std::unique_lock<std::mutex> lock(m);
            ++waiters;
            cv.wait(lock, [this] { return count == 0; });
            --waiters;
        }

        void arrive_and_wait(std::ptrdiff_t n = 1) {
            std::unique_lock<std::mutex> lock(m);
            if (n > count)
                throw std::logic_error("moveable_latch: count_down below zero");
            count -= n;
            if (count == 0) {
                cv.notify_all();
                return;
            }
            ++waiters;
            cv.wait(lock, [this] { return count == 0; });
            --waiters;
        }

        // Remaining count - a snapshot, for testing and diagnostics
        std::ptrdiff_t remaining() const {
            std::lock_guard<std::mutex> lock(m);
            return count;
        }
    };

} // namespace snicholls

#endif /* moveable_latch_hpp */
