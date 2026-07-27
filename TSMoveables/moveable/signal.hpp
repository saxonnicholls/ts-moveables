//
//  moveable_signal.hpp
//  TSMoveables
//
//  Created by Saxon Nicholls on 21/7/2026.
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables
//

#ifndef moveable_signal_hpp
#define moveable_signal_hpp

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace snicholls
{
    namespace detail
    {
        // Shared between a slot and its connection handles. Non-template, so
        // one connection type serves every signal instantiation.
        struct slot_state {
            std::atomic<bool> alive{true};
            std::weak_ptr<const void> track;
            bool tracked = false;
        };
    } // namespace detail

    // A handle to one connection. Copyable; all copies refer to the same slot.
    // Valid to use after the signal itself has been destroyed or moved.
    class connection {
        std::weak_ptr<detail::slot_state> st_;

    public:
        connection() noexcept = default;
        explicit connection(std::shared_ptr<detail::slot_state> st) noexcept : st_(std::move(st)) {}

        // Idempotent. A call already in flight on another thread may still
        // complete - the same guarantee Boost.Signals2 documents.
        void disconnect() noexcept {
            if (auto s = st_.lock())
                s->alive.store(false, std::memory_order_release);
            st_.reset();
        }

        bool connected() const noexcept {
            auto s = st_.lock();
            return s && s->alive.load(std::memory_order_acquire);
        }
    };

    // RAII: disconnects when it goes out of scope
    class scoped_connection {
        connection c_;

    public:
        scoped_connection() noexcept = default;
        scoped_connection(connection c) noexcept : c_(std::move(c)) {}

        scoped_connection(const scoped_connection&) = delete;
        scoped_connection& operator=(const scoped_connection&) = delete;

        scoped_connection(scoped_connection&& other) noexcept : c_(std::exchange(other.c_, connection{})) {}
        scoped_connection& operator=(scoped_connection&& other) noexcept {
            if (this != &other) {
                c_.disconnect();
                c_ = std::exchange(other.c_, connection{});
            }
            return *this;
        }

        ~scoped_connection() { c_.disconnect(); }

        connection release() noexcept { return std::exchange(c_, connection{}); }
        bool connected() const noexcept { return c_.connected(); }
        void disconnect() noexcept { c_.disconnect(); }
    };

    // Thread safe signal/slot
    //
    // Everything - connect, disconnect, emit, slot execution - may happen from
    // any thread. The design rules:
    //
    //  - Emission never holds the signal's lock while calling user code. An
    //    emit takes one brief lock to grab an immutable snapshot of the slot
    //    list (a shared_ptr copy), then invokes without it. Slots may freely
    //    connect, disconnect, or re-emit - deadlock-free by construction, and
    //    the emit path allocates nothing.
    //  - Slots are invoked in connection order (several popular libraries do
    //    not promise this).
    //  - A slot connected during an emission is not called in that emission;
    //    a slot disconnected during an emission is skipped for the rest of it,
    //    though a call already started may complete.
    //  - Lifetime tracking: connect(weak_or_shared_ptr, f) auto-disconnects
    //    when the object dies, and the object is kept alive for the duration
    //    of any call already dispatched to it.
    //  - Disconnected slots are skipped immediately and their storage is
    //    reclaimed on the next connect / disconnect_all.
    //
    // Movability - the reason a signal belongs in this library: connections
    // bind to the signal's shared internal state, never to the signal object's
    // address, so a signal member moves freely with its owner and every
    // connection stays live. Copying is deleted (two signals sharing slots is
    // a bug, not a feature). A moved-from signal is empty: emit and connect on
    // it are safe no-ops.
    template <typename... Args>
    class moveable_signal {

        struct slot_entry {
            std::shared_ptr<detail::slot_state> state;
            std::function<void(Args...)> fn;
        };
        using slot_vector = std::vector<slot_entry>;

        struct shared {
            std::mutex m;
            std::shared_ptr<const slot_vector> slots = std::make_shared<slot_vector>();
        };

        std::shared_ptr<shared> s_ = std::make_shared<shared>();

        // Requires the lock: copy the live entries
        static slot_vector live_copy(const slot_vector& v) {
            slot_vector out;
            out.reserve(v.size() + 1);
            for (const slot_entry& e : v)
                if (e.state->alive.load(std::memory_order_acquire))
                    out.push_back(e);
            return out;
        }

        template <typename F>
        connection connect_entry(std::shared_ptr<detail::slot_state> st, F&& f) {
            if (!s_)
                return connection{};
            std::lock_guard<std::mutex> lock(s_->m);
            slot_vector v = live_copy(*s_->slots);
            v.push_back(slot_entry{st, std::function<void(Args...)>(std::forward<F>(f))});
            s_->slots = std::make_shared<const slot_vector>(std::move(v));
            return connection(std::move(st));
        }

    public:
        moveable_signal() = default;

        moveable_signal(const moveable_signal&) = delete;
        moveable_signal& operator=(const moveable_signal&) = delete;

        moveable_signal(moveable_signal&&) noexcept = default;
        moveable_signal& operator=(moveable_signal&&) noexcept = default;

        // Connect a callable f(Args...)
        template <typename F>
        connection connect(F&& f) {
            return connect_entry(std::make_shared<detail::slot_state>(), std::forward<F>(f));
        }

        // Connect with lifetime tracking: the slot is skipped and dropped once
        // the tracked object expires, and the object is kept alive while a
        // dispatched call runs
        template <typename F>
        connection connect(std::weak_ptr<const void> track, F&& f) {
            auto st = std::make_shared<detail::slot_state>();
            st->track = std::move(track);
            st->tracked = true;
            return connect_entry(std::move(st), std::forward<F>(f));
        }

        // Convenience: member function on a shared_ptr-owned object, tracked
        template <typename O, typename R>
        connection connect(const std::shared_ptr<O>& obj, R (O::*mf)(Args...)) {
            O* raw = obj.get();
            return connect(std::weak_ptr<const void>(obj),
                           [raw, mf](Args... a) { (raw->*mf)(a...); });
        }

        // Emit. Slots run on the emitting thread, in connection order.
        void operator()(Args... args) const {
            if (!s_)
                return;
            std::shared_ptr<const slot_vector> snap;
            {
                std::lock_guard<std::mutex> lock(s_->m);
                snap = s_->slots;
            }
            for (const slot_entry& e : *snap) {
                detail::slot_state& st = *e.state;
                if (!st.alive.load(std::memory_order_acquire))
                    continue;
                if (st.tracked) {
                    const auto keep = st.track.lock();   // holds the target alive for the call
                    if (!keep) {
                        st.alive.store(false, std::memory_order_release);
                        continue;
                    }
                    e.fn(args...);
                } else {
                    e.fn(args...);
                }
            }
        }

        void disconnect_all() {
            if (!s_)
                return;
            std::lock_guard<std::mutex> lock(s_->m);
            for (const slot_entry& e : *s_->slots)
                e.state->alive.store(false, std::memory_order_release);
            s_->slots = std::make_shared<const slot_vector>();
        }

        // Currently connected (live) slots - a snapshot
        std::size_t slot_count() const {
            if (!s_)
                return 0;
            std::shared_ptr<const slot_vector> snap;
            {
                std::lock_guard<std::mutex> lock(s_->m);
                snap = s_->slots;
            }
            std::size_t n = 0;
            for (const slot_entry& e : *snap)
                if (e.state->alive.load(std::memory_order_acquire))
                    ++n;
            return n;
        }
    };

} // namespace snicholls

#endif /* moveable_signal_hpp */
