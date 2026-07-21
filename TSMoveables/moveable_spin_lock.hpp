//
//  moveable_spin_lock.hpp
//  TSMoveables
//
//  Created by Saxon Nicholls on 21/7/2026.
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables
//

#ifndef moveable_spin_lock_hpp
#define moveable_spin_lock_hpp

#include <atomic>
#include <thread>
#include <stdexcept>

namespace snicholls
{
    // Moveable spin lock
    //
    // The standard library has no spin lock; the usual hand-rolled one sits on
    // std::atomic_flag, which is immovable. This one's entire state is a single
    // atomic bool, so it can move under the same contract as moveable_mutex:
    // a move verifies via a try_lock probe that nobody holds the source (or the
    // destination, on assignment) and throws std::runtime_error otherwise. The
    // moved-from lock remains valid and unlocked.
    //
    // Satisfies Lockable, so it works with std::lock_guard, std::unique_lock,
    // std::scoped_lock and snicholls::moveable_condition_variable_any.
    //
    // Implementation is test-and-test-and-set, yielding after a bounded number
    // of spins so a contended lock does not burn a whole core.
    struct moveable_spin_lock {

    private:
        std::atomic<bool> held{false};

        static void ensure_quiescent(moveable_spin_lock& lock) {
            if (!lock.try_lock())
                throw std::runtime_error("moveable_spin_lock: moving a lock that is held");
            lock.unlock();
        }

    public:
        moveable_spin_lock() noexcept = default;

        moveable_spin_lock(const moveable_spin_lock&) = delete;
        moveable_spin_lock& operator=(const moveable_spin_lock&) = delete;

        moveable_spin_lock(moveable_spin_lock&& other) {
            ensure_quiescent(other);
        }

        moveable_spin_lock& operator=(moveable_spin_lock&& other) {
            if (this != &other) {
                ensure_quiescent(*this);
                ensure_quiescent(other);
            }
            return *this;
        }

        void lock() noexcept {
            for (;;) {
                if (!held.exchange(true, std::memory_order_acquire))
                    return;
                int spins = 0;
                while (held.load(std::memory_order_relaxed)) {
                    if (++spins == 1024) {
                        spins = 0;
                        std::this_thread::yield();
                    }
                }
            }
        }

        bool try_lock() noexcept {
            return !held.load(std::memory_order_relaxed) &&
                   !held.exchange(true, std::memory_order_acquire);
        }

        void unlock() noexcept {
            held.store(false, std::memory_order_release);
        }
    };

} // namespace snicholls

#endif /* moveable_spin_lock_hpp */
