//
//  moveable_mutex.hpp
//  TSMoveables
//
//  Created by Saxon Nicholls on 17/4/2024.
//
//  Copyright 2024 Saxon Herschel Nicholls
//
//  Thread Safe Moveables
//

#ifndef moveable_mutex_hpp
#define moveable_mutex_hpp

#include <mutex>
#include <shared_mutex>
#include <chrono>
#include <stdexcept>

namespace snicholls
{
    // Moveable mutex
    // Prefer composition over inheritance - std::mutex does not have a virtual destructor
    //
    // An unlocked mutex carries no observable state, so "moving" one simply means:
    // verify nobody holds the source (or the destination, on assignment) and start
    // fresh. The verification is a try_lock probe - if any thread holds the mutex,
    // exclusively or shared, the move throws std::runtime_error instead of
    // silently corrupting a lock somebody else is relying on. That is the integrity
    // guarantee: a move either happens on a quiescent mutex or not at all.
    //
    // Caveats, as with any such probe:
    //  - For recursive mutexes the probe cannot detect locks held by the moving
    //    thread itself (try_lock just recurses); moving a mutex you yourself hold
    //    is still a logic error.
    //  - A thread may lock the mutex immediately after the probe. The probe makes
    //    misuse loud; it does not make concurrent move + lock a sensible program.
    //
    // All of the standard mutex API is forwarded. Members are only instantiated
    // when called, so moveable_mutex<std::mutex> compiles even though std::mutex
    // has no lock_shared - exactly like the standard library's own wrappers.
    template <typename M = std::mutex>
    struct moveable_mutex {

        using mutex_type = M;
        using lock_guard_type = std::lock_guard<moveable_mutex>;

    private:
        M m;

        static void ensure_quiescent(M& mtx) {
            if (!mtx.try_lock())
                throw std::runtime_error("moveable_mutex: moving a mutex that is locked");
            mtx.unlock();
        }

    public:
        moveable_mutex() = default;

        moveable_mutex(const moveable_mutex&) = delete;
        moveable_mutex& operator=(const moveable_mutex&) = delete;

        moveable_mutex(moveable_mutex&& other) : m() {
            ensure_quiescent(other.m);
        }

        moveable_mutex& operator=(moveable_mutex&& other) {
            if (this != &other) {
                ensure_quiescent(m);
                ensure_quiescent(other.m);
            }
            return *this;
        }

        ~moveable_mutex() = default;

        // Exclusive locking
        void lock()     { m.lock(); }
        bool try_lock() { return m.try_lock(); }
        void unlock()   { m.unlock(); }

        // Timed locking - std::timed_mutex, std::recursive_timed_mutex, std::shared_timed_mutex
        template <typename Rep, typename Period>
        bool try_lock_for(const std::chrono::duration<Rep, Period>& d) { return m.try_lock_for(d); }

        template <typename Clock, typename Duration>
        bool try_lock_until(const std::chrono::time_point<Clock, Duration>& t) { return m.try_lock_until(t); }

        // Shared locking - std::shared_mutex, std::shared_timed_mutex
        void lock_shared()     { m.lock_shared(); }
        bool try_lock_shared() { return m.try_lock_shared(); }
        void unlock_shared()   { m.unlock_shared(); }

        template <typename Rep, typename Period>
        bool try_lock_shared_for(const std::chrono::duration<Rep, Period>& d) { return m.try_lock_shared_for(d); }

        template <typename Clock, typename Duration>
        bool try_lock_shared_until(const std::chrono::time_point<Clock, Duration>& t) { return m.try_lock_shared_until(t); }

        // Access to the wrapped mutex, e.g. for std::condition_variable which
        // insists on std::unique_lock<std::mutex>
        M& native() noexcept { return m; }
        operator M&() noexcept { return m; }
    };

    // Type Aliases
    // Mutex
    using moveable_recursive_mutex = moveable_mutex< std::recursive_mutex >;
    using moveable_shared_mutex = moveable_mutex< std::shared_mutex >;
    using moveable_timed_mutex = moveable_mutex< std::timed_mutex >;
    using moveable_recursive_timed_mutex = moveable_mutex< std::recursive_timed_mutex >;
    using moveable_shared_timed_mutex = moveable_mutex< std::shared_timed_mutex >;

} // namespace snicholls


#endif /* moveable_mutex_hpp */
