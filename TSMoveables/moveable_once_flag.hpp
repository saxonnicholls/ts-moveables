//
//  moveable_once_flag.hpp
//  TSMoveables
//
//  Created by Saxon Nicholls on 21/7/2026.
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables
//

#ifndef moveable_once_flag_hpp
#define moveable_once_flag_hpp

#include <atomic>
#include <mutex>
#include <functional>
#include <stdexcept>
#include <utility>

namespace snicholls
{
    // Moveable once flag
    //
    // std::once_flag is immovable and its state cannot be observed, so we keep
    // the state ourselves: an atomic "done" flag plus a mutex serialising the
    // slow path. Semantics match std::call_once - exactly one caller runs the
    // callable; if it throws, "done" stays false and the next caller retries.
    //
    // Copy and move both transfer the observable state (has the callable run?),
    // so a moved object remembers that its work was already done. Moving or
    // copying while another thread is mid-call throws std::runtime_error.
    struct moveable_once_flag {

    private:
        std::atomic<bool> done{false};
        mutable std::mutex m;

        static void ensure_quiescent(std::mutex& mtx) {
            if (!mtx.try_lock())
                throw std::runtime_error("moveable_once_flag: moving while a call_once is in progress");
            mtx.unlock();
        }

    public:
        moveable_once_flag() = default;

        moveable_once_flag(const moveable_once_flag& cpy) {
            ensure_quiescent(cpy.m);
            done.store(cpy.done.load());
        }

        moveable_once_flag(moveable_once_flag&& mve) {
            ensure_quiescent(mve.m);
            done.store(mve.done.load());
        }

        moveable_once_flag& operator=(const moveable_once_flag& other) {
            if (this != &other) {
                ensure_quiescent(m);
                ensure_quiescent(other.m);
                done.store(other.done.load());
            }
            return *this;
        }

        moveable_once_flag& operator=(moveable_once_flag&& other) {
            if (this != &other) {
                ensure_quiescent(m);
                ensure_quiescent(other.m);
                done.store(other.done.load());
            }
            return *this;
        }

        template <typename Callable, typename... Args>
        void call_once(Callable&& f, Args&&... args) {
            if (done.load(std::memory_order_acquire))
                return;
            std::lock_guard<std::mutex> lock(m);
            if (done.load(std::memory_order_relaxed))
                return;
            std::invoke(std::forward<Callable>(f), std::forward<Args>(args)...);
            done.store(true, std::memory_order_release);
        }

        // Has the callable successfully run?
        bool called() const noexcept { return done.load(std::memory_order_acquire); }
    };

    template <typename Callable, typename... Args>
    void call_once(moveable_once_flag& flag, Callable&& f, Args&&... args) {
        flag.call_once(std::forward<Callable>(f), std::forward<Args>(args)...);
    }

} // namespace snicholls

#endif /* moveable_once_flag_hpp */
