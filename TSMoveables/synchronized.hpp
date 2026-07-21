//
//  synchronized.hpp
//  TSMoveables
//
//  Created by Saxon Nicholls on 21/7/2026.
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables
//

#ifndef synchronized_hpp
#define synchronized_hpp

#include <mutex>
#include <shared_mutex>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "moveable_condition_variable.hpp"

namespace snicholls
{
    // synchronized<T> - a value bonded to its mutex
    //
    // The only way to reach the T is through a closure that runs with the lock
    // held, which structurally eliminates the classic misuse: lock, return a
    // reference, touch it after unlock. Closures that try to return a
    // reference are rejected at compile time for the same reason.
    //
    //     snicholls::synchronized<std::vector<int>> items;
    //     items.with_lock([](auto& v) { v.push_back(42); });
    //     auto n = items.with_lock([](const auto& v) { return v.size(); });
    //
    // M may be any BasicLockable - std::mutex (the default), std::shared_mutex
    // (enabling with_read_lock), std::recursive_mutex (allowing nested
    // with_lock on the same object), or any of this library's moveable
    // mutexes, including moveable_spin_lock for very short critical sections.
    //
    // Copy and move are themselves thread safe with respect to the source: the
    // source's lock is held while its value is read or moved out, and both
    // sides are locked (deadlock-free, via std::scoped_lock) for assignment.
    // The mutex itself never moves - each synchronized owns its own for life -
    // so unlike the raw primitives no quiescent-move contract is needed here.
    // A moved-from synchronized holds a moved-from T and remains fully usable.
    //
    // T can itself be heterogeneous - synchronized<std::variant<...>> for a
    // closed set of alternatives, synchronized<std::tuple<...>> for a fixed
    // bundle, synchronized<std::vector<std::any>> for an open bag - giving a
    // thread-safe heterogeneous container with no further machinery.
    template <typename T, typename M = std::mutex>
    struct synchronized {

        using value_type = T;
        using mutex_type = M;

    private:
        mutable M m;
        T value;

        // Lock-held delegates so copy and move construction hold the source's
        // lock for the duration
        synchronized(const synchronized& other, const std::lock_guard<M>&) : value(other.value) {}
        synchronized(synchronized&& other, const std::lock_guard<M>&) : value(std::move(other.value)) {}

    protected:
        M& mtx() const noexcept { return m; }
        T& value_unlocked() noexcept { return value; }
        const T& value_unlocked() const noexcept { return value; }

    public:
        synchronized() = default;
        synchronized(T v) : value(std::move(v)) {}

        template <typename... Args>
        explicit synchronized(std::in_place_t, Args&&... args) : value(std::forward<Args>(args)...) {}

        synchronized(const synchronized& other) : synchronized(other, std::lock_guard<M>(other.m)) {}
        synchronized(synchronized&& other) : synchronized(std::move(other), std::lock_guard<M>(other.m)) {}

        synchronized& operator=(const synchronized& other) {
            if (this != &other) {
                std::scoped_lock lock(m, other.m);
                value = other.value;
            }
            return *this;
        }

        synchronized& operator=(synchronized&& other) {
            if (this != &other) {
                std::scoped_lock lock(m, other.m);
                value = std::move(other.value);
            }
            return *this;
        }

        synchronized& operator=(T v) {
            store(std::move(v));
            return *this;
        }

        ~synchronized() = default;

        // The access paths - the closure runs with the lock held
        template <typename F>
        decltype(auto) with_lock(F&& f) {
            static_assert(!std::is_reference_v<std::invoke_result_t<F, T&>>,
                          "synchronized: the closure must not return a reference - it would escape the lock");
            std::lock_guard<M> lock(m);
            return std::invoke(std::forward<F>(f), value);
        }

        template <typename F>
        decltype(auto) with_lock(F&& f) const {
            static_assert(!std::is_reference_v<std::invoke_result_t<F, const T&>>,
                          "synchronized: the closure must not return a reference - it would escape the lock");
            std::lock_guard<M> lock(m);
            return std::invoke(std::forward<F>(f), value);
        }

        template <typename F>
        decltype(auto) with_write_lock(F&& f) {
            return with_lock(std::forward<F>(f));
        }

        // Shared (reader) access - for M with lock_shared, e.g. std::shared_mutex
        // or moveable_shared_mutex; only instantiated when called
        template <typename F>
        decltype(auto) with_read_lock(F&& f) const {
            static_assert(!std::is_reference_v<std::invoke_result_t<F, const T&>>,
                          "synchronized: the closure must not return a reference - it would escape the lock");
            std::shared_lock<M> lock(m);
            return std::invoke(std::forward<F>(f), value);
        }

        // Whole-value conveniences
        T load() const {
            std::lock_guard<M> lock(m);
            return value;
        }

        void store(T v) {
            std::lock_guard<M> lock(m);
            value = std::move(v);
        }

        T exchange(T v) {
            std::lock_guard<M> lock(m);
            T old = std::move(value);
            value = std::move(v);
            return old;
        }
    };

    template <typename T> synchronized(T) -> synchronized<T>;

    // synchronized_waitable<T> - synchronized<T> plus condition waiting
    //
    // Adds the producer/consumer vocabulary: update() mutates and wakes every
    // waiter; wait() blocks until a predicate over the value holds; wait_then()
    // blocks and then consumes under the same lock. Waiting is tracked with
    // moveable_condition_variable_any, so moving while threads are blocked
    // throws std::runtime_error - the quiescent contract applies exactly where
    // it must (waiters cannot be transferred), while plain synchronized needs
    // no contract at all.
    template <typename T, typename M = std::mutex>
    struct synchronized_waitable : synchronized<T, M> {

        using base_type = synchronized<T, M>;
        using value_type = T;
        using mutex_type = M;

    private:
        mutable moveable_condition_variable_any cv;

        static synchronized_waitable&& ensure_no_waiters(synchronized_waitable&& other) {
            if (other.cv.waiting() != 0)
                throw std::runtime_error("synchronized_waitable: moving while threads are waiting");
            return std::move(other);
        }

    public:
        using base_type::base_type;

        synchronized_waitable() = default;

        synchronized_waitable(const synchronized_waitable& other) : base_type(other) {}
        synchronized_waitable(synchronized_waitable&& other) : base_type(ensure_no_waiters(std::move(other))) {}

        synchronized_waitable& operator=(const synchronized_waitable& other) {
            base_type::operator=(other);
            return *this;
        }

        synchronized_waitable& operator=(synchronized_waitable&& other) {
            if (this != &other) {
                if (cv.waiting() != 0 || other.cv.waiting() != 0)
                    throw std::runtime_error("synchronized_waitable: moving while threads are waiting");
                base_type::operator=(std::move(other));
            }
            return *this;
        }

        // Mutate then wake every waiter - the usual producer step
        template <typename F>
        decltype(auto) update(F&& f) {
            struct notifier {
                moveable_condition_variable_any& cv;
                ~notifier() { cv.notify_all(); }
            } n{cv};
            return this->with_lock(std::forward<F>(f));
        }

        void notify_one() noexcept { cv.notify_one(); }
        void notify_all() noexcept { cv.notify_all(); }

        // Block until pred(value) holds
        template <typename Pred>
        void wait(Pred pred) const {
            std::unique_lock<M> lock(this->mtx());
            cv.wait(lock, [&] { return pred(this->value_unlocked()); });
        }

        template <typename Rep, typename Period, typename Pred>
        bool wait_for(const std::chrono::duration<Rep, Period>& d, Pred pred) const {
            std::unique_lock<M> lock(this->mtx());
            return cv.wait_for(lock, d, [&] { return pred(this->value_unlocked()); });
        }

        template <typename Clock, typename Duration, typename Pred>
        bool wait_until(const std::chrono::time_point<Clock, Duration>& t, Pred pred) const {
            std::unique_lock<M> lock(this->mtx());
            return cv.wait_until(lock, t, [&] { return pred(this->value_unlocked()); });
        }

        // Block until pred(value) holds, then run f under the same lock -
        // the usual consumer step
        template <typename Pred, typename F>
        decltype(auto) wait_then(Pred pred, F&& f) {
            static_assert(!std::is_reference_v<std::invoke_result_t<F, T&>>,
                          "synchronized: the closure must not return a reference - it would escape the lock");
            std::unique_lock<M> lock(this->mtx());
            cv.wait(lock, [&] { return pred(std::as_const(*this).value_unlocked()); });
            return std::invoke(std::forward<F>(f), this->value_unlocked());
        }

        // Number of threads currently blocked in a wait on this object
        int waiting() const noexcept { return cv.waiting(); }
    };

    template <typename T> synchronized_waitable(T) -> synchronized_waitable<T>;

} // namespace snicholls

#endif /* synchronized_hpp */
