//
//  event_loop.hpp
//  TSMoveables
//
//  Created by Saxon Nicholls on 22/7/2026.
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables
//

#ifndef event_loop_hpp
#define event_loop_hpp

// Phase 1 is a POSIX readiness reactor (epoll / kqueue / poll). Windows
// readiness IO is a different animal - IOCP is a proactor, and bridging the
// two models is precisely why Asio is complicated. For IOCP-grade Windows IO
// use Asio; we say so plainly (see FUTURE_DIRECTIONS). On Windows this header
// compiles to nothing and SNICHOLLS_HAS_EVENT_LOOP is 0.
#if defined(_WIN32)
#define SNICHOLLS_HAS_EVENT_LOOP 0
#else
#define SNICHOLLS_HAS_EVENT_LOOP 1

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#if !defined(SNICHOLLS_EVENT_LOOP_FORCE_POLL) && defined(__linux__)
#define SNICHOLLS_EL_BACKEND_EPOLL 1
#include <sys/epoll.h>
#elif !defined(SNICHOLLS_EVENT_LOOP_FORCE_POLL) && \
    (defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
     defined(__OpenBSD__) || defined(__DragonFly__))
#define SNICHOLLS_EL_BACKEND_KQUEUE 1
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#else
#define SNICHOLLS_EL_BACKEND_POLL 1
#include <poll.h>
#endif

#include "moveable_signal.hpp"
#include "mpmc_queue.hpp"

namespace snicholls
{
    // What a watch is interested in
    enum class fd_interest : unsigned { none = 0, read = 1, write = 2, read_write = 3 };

    constexpr fd_interest operator|(fd_interest a, fd_interest b) noexcept {
        return static_cast<fd_interest>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
    }
    constexpr bool has(fd_interest v, fd_interest f) noexcept {
        return (static_cast<unsigned>(v) & static_cast<unsigned>(f)) != 0;
    }

    // What the loop just dispatched - the journal tap payload. Everything the
    // loop delivers (fd events, timer fires, posted tasks) is announced here
    // first, which is what makes a session recordable and replayable.
    struct dispatch_info {
        enum class kind { task, timer, readable, writable, fd_error };
        kind what;
        int fd;                     // -1 unless an fd event
        std::uint64_t timer_id;     // 0 unless a timer fire
    };

    namespace detail
    {
        struct el_event {
            int fd;
            bool readable;
            bool writable;
            bool error;
        };

        // One small readiness backend per platform, all level-triggered.
        class el_poller {
        public:
            el_poller() {
#if defined(SNICHOLLS_EL_BACKEND_EPOLL)
                handle_ = ::epoll_create1(EPOLL_CLOEXEC);
                if (handle_ < 0)
                    throw std::runtime_error("event_loop: epoll_create1 failed");
#elif defined(SNICHOLLS_EL_BACKEND_KQUEUE)
                handle_ = ::kqueue();
                if (handle_ < 0)
                    throw std::runtime_error("event_loop: kqueue failed");
#endif
            }

            ~el_poller() {
#if !defined(SNICHOLLS_EL_BACKEND_POLL)
                if (handle_ >= 0)
                    ::close(handle_);
#endif
            }

            el_poller(const el_poller&) = delete;
            el_poller& operator=(const el_poller&) = delete;

            void add(int fd, bool r, bool w) {
#if defined(SNICHOLLS_EL_BACKEND_EPOLL)
                ctl(EPOLL_CTL_ADD, fd, r, w);
#elif defined(SNICHOLLS_EL_BACKEND_KQUEUE)
                apply(fd, EVFILT_READ, r, true);
                apply(fd, EVFILT_WRITE, w, true);
#else
                interest_[fd] = mask(r, w);
#endif
            }

            void modify(int fd, bool r, bool w) {
#if defined(SNICHOLLS_EL_BACKEND_EPOLL)
                ctl(EPOLL_CTL_MOD, fd, r, w);
#elif defined(SNICHOLLS_EL_BACKEND_KQUEUE)
                apply(fd, EVFILT_READ, r, false);
                apply(fd, EVFILT_WRITE, w, false);
#else
                interest_[fd] = mask(r, w);
#endif
            }

            void remove(int fd) noexcept {
#if defined(SNICHOLLS_EL_BACKEND_EPOLL)
                ::epoll_ctl(handle_, EPOLL_CTL_DEL, fd, nullptr);   // errors ignored: fd may be closed
#elif defined(SNICHOLLS_EL_BACKEND_KQUEUE)
                struct kevent ev;
                EV_SET(&ev, static_cast<uintptr_t>(fd), EVFILT_READ, EV_DELETE, 0, 0, nullptr);
                ::kevent(handle_, &ev, 1, nullptr, 0, nullptr);
                EV_SET(&ev, static_cast<uintptr_t>(fd), EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
                ::kevent(handle_, &ev, 1, nullptr, 0, nullptr);
#else
                interest_.erase(fd);
#endif
            }

            // timeout_ms < 0 blocks indefinitely
            void wait(std::vector<el_event>& out, int timeout_ms) {
#if defined(SNICHOLLS_EL_BACKEND_EPOLL)
                epoll_event evs[64];
                const int n = ::epoll_wait(handle_, evs, 64, timeout_ms);
                for (int i = 0; i < n; ++i)
                    out.push_back(el_event{evs[i].data.fd,
                                           (evs[i].events & EPOLLIN) != 0,
                                           (evs[i].events & EPOLLOUT) != 0,
                                           (evs[i].events & (EPOLLERR | EPOLLHUP)) != 0});
#elif defined(SNICHOLLS_EL_BACKEND_KQUEUE)
                struct kevent evs[64];
                struct timespec ts;
                struct timespec* tsp = nullptr;
                if (timeout_ms >= 0) {
                    ts.tv_sec = timeout_ms / 1000;
                    ts.tv_nsec = static_cast<long>(timeout_ms % 1000) * 1'000'000L;
                    tsp = &ts;
                }
                const int n = ::kevent(handle_, nullptr, 0, evs, 64, tsp);
                for (int i = 0; i < n; ++i)
                    out.push_back(el_event{static_cast<int>(evs[i].ident),
                                           evs[i].filter == EVFILT_READ,
                                           evs[i].filter == EVFILT_WRITE,
                                           (evs[i].flags & (EV_ERROR | EV_EOF)) != 0});
#else
                fds_.clear();
                for (const auto& [fd, m] : interest_)
                    fds_.push_back(pollfd{fd, m, 0});
                const int n = ::poll(fds_.data(), static_cast<nfds_t>(fds_.size()), timeout_ms);
                if (n > 0)
                    for (const pollfd& p : fds_)
                        if (p.revents != 0)
                            out.push_back(el_event{p.fd,
                                                   (p.revents & POLLIN) != 0,
                                                   (p.revents & POLLOUT) != 0,
                                                   (p.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0});
#endif
            }

        private:
#if defined(SNICHOLLS_EL_BACKEND_EPOLL)
            int handle_ = -1;
            void ctl(int op, int fd, bool r, bool w) {
                epoll_event ev{};
                ev.events = (r ? static_cast<std::uint32_t>(EPOLLIN) : 0u) |
                            (w ? static_cast<std::uint32_t>(EPOLLOUT) : 0u);
                ev.data.fd = fd;
                if (::epoll_ctl(handle_, op, fd, &ev) != 0)
                    throw std::runtime_error("event_loop: epoll_ctl failed");
            }
#elif defined(SNICHOLLS_EL_BACKEND_KQUEUE)
            int handle_ = -1;
            void apply(int fd, int filter, bool on, bool adding) {
                struct kevent ev;
                EV_SET(&ev, static_cast<uintptr_t>(fd), filter,
                       on ? (EV_ADD | EV_ENABLE) : EV_DELETE, 0, 0, nullptr);
                const int rc = ::kevent(handle_, &ev, 1, nullptr, 0, nullptr);
                if (rc != 0 && on && adding)
                    throw std::runtime_error("event_loop: kevent add failed");
                // EV_DELETE of a filter that was never added reports ENOENT - fine
            }
#else
            std::unordered_map<int, short> interest_;
            std::vector<pollfd> fds_;
            static short mask(bool r, bool w) noexcept {
                return static_cast<short>((r ? POLLIN : 0) | (w ? POLLOUT : 0));
            }
#endif
        };
    } // namespace detail

    // Event loop - a clean, typed, honest POSIX reactor
    //
    // Dispatch is moveable_signal: fd readiness, timer fires and posted tasks
    // all flow through typed signal emission, which makes handler lifetimes
    // safe by construction (weak tracking, RAII connections), keeps delivery
    // in connection order, and tolerates handlers that add or remove watches
    // mid-dispatch. Every delivery is announced on the on_dispatch() tap
    // first, which is what makes a session recordable and replayable.
    //
    // Thirty years of event-loop failure modes, addressed by name:
    //   - use-after-free handlers: connect with lifetime tracking, or hold a
    //     scoped_connection - dead receivers are skipped, by construction
    //   - wrong-thread mutation: one thread runs the loop; watches and timers
    //     are created on the loop thread (or before run); violations throw
    //     std::logic_error instead of racing. post() is the one door in from
    //     other threads; timer::cancel() is safe from any thread; destroying
    //     an fd_watch on a foreign thread marshals itself through post().
    //   - lost wakeups: post() enqueues to a bounded mpmc_queue and writes a
    //     self-pipe byte; the pipe is non-blocking and fully drained.
    //   - reentrancy corruption: per-event state lookup is fresh, emission is
    //     snapshot-based - handlers may create and destroy watches freely.
    //   - timer drift and post-stall bursts: periodic timers reschedule
    //     drift-free, clamped after stalls so they never burst to catch up.
    //   - teardown disasters: the loop handle is moveable (heap-stable core);
    //     watches and timers outlive or predecease the loop safely; run()
    //     keeps the core alive even if the handle is destroyed mid-run.
    //
    // Semantics: level-triggered, single loop thread, handler exceptions
    // propagate out of run()/run_once() leaving the loop stopped but usable.
    // fd-number reuse within one dispatch batch can misattribute a stale event
    // to a new watch on the same number (every readiness loop shares this);
    // avoid closing-and-rewatching the same fd inside one handler pass.
    class event_loop {

        struct fd_state {
            int fd = -1;
            fd_interest interest = fd_interest::none;
            moveable_signal<> on_readable;
            moveable_signal<> on_writable;
            moveable_signal<> on_error;
        };

        struct timer_state {
            std::uint64_t id = 0;
            std::chrono::nanoseconds period{0};     // 0: one-shot
            std::atomic<bool> alive{true};
            moveable_signal<> on_fire;
        };

        struct heap_entry {
            std::chrono::steady_clock::time_point deadline;
            std::uint64_t seq;
            std::shared_ptr<timer_state> t;
        };
        struct heap_later {
            bool operator()(const heap_entry& a, const heap_entry& b) const noexcept {
                if (a.deadline != b.deadline)
                    return a.deadline > b.deadline;
                return a.seq > b.seq;
            }
        };

        struct core {
            detail::el_poller poller;
            int wake_read = -1, wake_write = -1;
            std::unordered_map<int, std::shared_ptr<fd_state>> fds;
            std::priority_queue<heap_entry, std::vector<heap_entry>, heap_later> timers;
            std::uint64_t next_seq = 0;
            std::uint64_t next_timer_id = 1;
            mpmc_queue<std::function<void()>> posted;
            std::atomic<bool> running{false};
            std::atomic<bool> stop_flag{false};
            std::atomic<std::thread::id> loop_thread{};
            moveable_signal<const dispatch_info&> tap;
            std::vector<detail::el_event> results;

            explicit core(std::size_t post_capacity) : posted(post_capacity) {
                int p[2];
                if (::pipe(p) != 0)
                    throw std::runtime_error("event_loop: pipe failed");
                wake_read = p[0];
                wake_write = p[1];
                ::fcntl(wake_read, F_SETFL, O_NONBLOCK);
                ::fcntl(wake_write, F_SETFL, O_NONBLOCK);
                poller.add(wake_read, true, false);
            }

            ~core() {
                if (wake_read >= 0)
                    ::close(wake_read);
                if (wake_write >= 0)
                    ::close(wake_write);
            }

            bool on_loop_thread() const noexcept {
                return std::this_thread::get_id() == loop_thread.load(std::memory_order_acquire);
            }

            void require_loop_thread(const char* what) const {
                if (running.load(std::memory_order_acquire) && !on_loop_thread())
                    throw std::logic_error(std::string("event_loop: ") + what +
                                           " must run on the loop thread (or before run())");
            }

            void wake() noexcept {
                char b = 'x';
                [[maybe_unused]] ssize_t r = ::write(wake_write, &b, 1);   // EAGAIN: already signalled
            }

            void enqueue_task(std::function<void()> t) {
                while (!posted.push(std::move(t)))          // bounded: backpressure when full
                    std::this_thread::yield();
                wake();
            }

            void remove_watch_if(int fd, const std::shared_ptr<fd_state>& st) noexcept {
                auto it = fds.find(fd);
                if (it != fds.end() && it->second == st) {
                    poller.remove(fd);
                    fds.erase(it);
                }
            }

            // One loop iteration. max_wait_ms < 0 means "block until work".
            bool iterate(long long max_wait_ms) {
                bool did = false;

                // 1. Posted tasks - a snapshot's worth, so a task that posts
                // more work cannot starve IO (the wake byte re-arms the poll)
                std::size_t budget = posted.size();
                std::function<void()> task;
                while (budget-- > 0 && posted.try_pop(task)) {
                    tap(dispatch_info{dispatch_info::kind::task, -1, 0});
                    task();
                    did = true;
                }

                // 2. Due timers
                auto now = std::chrono::steady_clock::now();
                while (!timers.empty() && timers.top().deadline <= now) {
                    heap_entry e = timers.top();
                    timers.pop();
                    if (!e.t->alive.load(std::memory_order_acquire))
                        continue;                           // lazily-cancelled
                    tap(dispatch_info{dispatch_info::kind::timer, -1, e.t->id});
                    e.t->on_fire();
                    did = true;
                    if (e.t->period.count() > 0 && e.t->alive.load(std::memory_order_acquire)) {
                        e.deadline += e.t->period;          // drift-free
                        if (e.deadline <= now)              // stalled: skip ahead, do not burst
                            e.deadline = now + e.t->period;
                        e.seq = next_seq++;
                        timers.push(e);
                    } else if (e.t->period.count() == 0) {
                        e.t->alive.store(false, std::memory_order_release);   // one-shot: spent
                    }
                }

                // 3. How long may the poller sleep?
                long long timeout_ms;
                if (stop_flag.load(std::memory_order_acquire)) {
                    timeout_ms = 0;
                } else if (!timers.empty()) {
                    const auto until = std::chrono::ceil<std::chrono::milliseconds>(
                                           timers.top().deadline - std::chrono::steady_clock::now())
                                           .count();
                    timeout_ms = until < 0 ? 0 : until;
                    if (max_wait_ms >= 0 && timeout_ms > max_wait_ms)
                        timeout_ms = max_wait_ms;
                } else {
                    timeout_ms = max_wait_ms;               // -1 blocks indefinitely
                }
                if (timeout_ms > 3'600'000)
                    timeout_ms = 3'600'000;                 // epoll takes int ms; 1h cap is plenty

                // 4. Wait and dispatch
                results.clear();
                poller.wait(results, static_cast<int>(timeout_ms));
                for (const detail::el_event& ev : results) {
                    if (ev.fd == wake_read) {
                        char buf[256];
                        while (::read(wake_read, buf, sizeof buf) > 0) {
                        }
                        continue;
                    }
                    auto it = fds.find(ev.fd);              // fresh lookup: a handler may have removed it
                    if (it == fds.end())
                        continue;
                    std::shared_ptr<fd_state> st = it->second;   // keep alive across emissions
                    if (ev.readable && has(st->interest, fd_interest::read)) {
                        tap(dispatch_info{dispatch_info::kind::readable, ev.fd, 0});
                        st->on_readable();
                        did = true;
                    }
                    // The handler may have destroyed or replaced this watch
                    auto again = fds.find(ev.fd);
                    if (again == fds.end() || again->second != st)
                        continue;
                    if (ev.writable && has(st->interest, fd_interest::write)) {
                        tap(dispatch_info{dispatch_info::kind::writable, ev.fd, 0});
                        st->on_writable();
                        did = true;
                    }
                    again = fds.find(ev.fd);
                    if (again == fds.end() || again->second != st)
                        continue;
                    if (ev.error) {
                        tap(dispatch_info{dispatch_info::kind::fd_error, ev.fd, 0});
                        st->on_error();
                        did = true;
                    }
                }
                return did;
            }
        };

        std::shared_ptr<core> c_;

        // Clears running state even if a handler throws
        struct run_guard {
            core& c;
            explicit run_guard(core& c_) : c(c_) {
                bool was = false;
                if (!c.running.compare_exchange_strong(was, true))
                    throw std::logic_error("event_loop: already running");
                c.loop_thread.store(std::this_thread::get_id(), std::memory_order_release);
            }
            ~run_guard() {
                c.loop_thread.store(std::thread::id{}, std::memory_order_release);
                c.running.store(false, std::memory_order_release);
            }
        };

    public:
        // A watch on one file descriptor. Moveable; destroying it unwatches.
        // Destroying from a foreign thread while the loop runs marshals the
        // unwatch through post() - it completes on the loop thread.
        class fd_watch {
            friend class event_loop;
            std::weak_ptr<core> c_;
            std::shared_ptr<fd_state> st_;

            fd_watch(std::weak_ptr<core> c, std::shared_ptr<fd_state> st)
                : c_(std::move(c)), st_(std::move(st)) {}

        public:
            fd_watch() = default;
            fd_watch(fd_watch&&) noexcept = default;
            fd_watch& operator=(fd_watch&& other) noexcept {
                if (this != &other) {
                    reset();
                    c_ = std::move(other.c_);
                    st_ = std::move(other.st_);
                }
                return *this;
            }
            fd_watch(const fd_watch&) = delete;
            fd_watch& operator=(const fd_watch&) = delete;
            ~fd_watch() { reset(); }

            void reset() noexcept {
                if (!st_)
                    return;
                std::shared_ptr<fd_state> st = std::move(st_);
                if (auto c = c_.lock()) {
                    if (!c->running.load(std::memory_order_acquire) || c->on_loop_thread()) {
                        c->remove_watch_if(st->fd, st);
                    } else {
                        // Foreign thread, loop live: marshal; st keeps the state
                        // alive until the loop processes it
                        std::weak_ptr<core> wc = c;
                        try {
                            c->enqueue_task([wc, st] {
                                if (auto cc = wc.lock())
                                    cc->remove_watch_if(st->fd, st);
                            });
                        } catch (...) {
                        }
                    }
                }
                c_.reset();
            }

            void set_interest(fd_interest interest) {
                auto c = c_.lock();
                if (!c || !st_)
                    throw std::logic_error("event_loop: fd_watch is empty");
                c->require_loop_thread("set_interest");
                st_->interest = interest;
                c->poller.modify(st_->fd, has(interest, fd_interest::read),
                                 has(interest, fd_interest::write));
            }

            moveable_signal<>& on_readable() { return state("on_readable").on_readable; }
            moveable_signal<>& on_writable() { return state("on_writable").on_writable; }
            moveable_signal<>& on_error() { return state("on_error").on_error; }

            int fd() const noexcept { return st_ ? st_->fd : -1; }
            explicit operator bool() const noexcept { return st_ != nullptr; }

        private:
            fd_state& state(const char* what) {
                if (!st_)
                    throw std::logic_error(std::string("event_loop: fd_watch is empty: ") + what);
                return *st_;
            }
        };

        // A one-shot or periodic timer. Moveable; cancel() (and destruction)
        // is safe from any thread.
        class timer {
            friend class event_loop;
            std::shared_ptr<timer_state> st_;

            explicit timer(std::shared_ptr<timer_state> st) : st_(std::move(st)) {}

        public:
            timer() = default;
            timer(timer&&) noexcept = default;
            timer& operator=(timer&& other) noexcept {
                if (this != &other) {
                    cancel();
                    st_ = std::move(other.st_);
                }
                return *this;
            }
            timer(const timer&) = delete;
            timer& operator=(const timer&) = delete;
            ~timer() { cancel(); }

            void cancel() noexcept {
                if (st_)
                    st_->alive.store(false, std::memory_order_release);
            }

            bool active() const noexcept {
                return st_ && st_->alive.load(std::memory_order_acquire);
            }

            std::uint64_t id() const noexcept { return st_ ? st_->id : 0; }

            moveable_signal<>& on_fire() {
                if (!st_)
                    throw std::logic_error("event_loop: timer is empty");
                return st_->on_fire;
            }
        };

        explicit event_loop(std::size_t post_capacity = 4096)
            : c_(std::make_shared<core>(post_capacity != 0 ? post_capacity : 4096)) {}

        event_loop(event_loop&&) noexcept = default;
        event_loop& operator=(event_loop&&) noexcept = default;
        event_loop(const event_loop&) = delete;
        event_loop& operator=(const event_loop&) = delete;

        // Watch an fd. Loop thread only while running (or any thread before run).
        fd_watch watch(int fd, fd_interest interest = fd_interest::read) {
            auto c = c_;
            if (fd < 0)
                throw std::invalid_argument("event_loop: bad fd");
            c->require_loop_thread("watch");
            if (c->fds.count(fd) != 0)
                throw std::logic_error("event_loop: fd already watched");
            auto st = std::make_shared<fd_state>();
            st->fd = fd;
            st->interest = interest;
            c->poller.add(fd, has(interest, fd_interest::read), has(interest, fd_interest::write));
            c->fds.emplace(fd, st);
            return fd_watch(c, std::move(st));
        }

        // One-shot timer. Loop thread only while running.
        template <typename Rep, typename Period>
        timer after(std::chrono::duration<Rep, Period> delay) {
            return make_timer(std::chrono::duration_cast<std::chrono::nanoseconds>(delay),
                              std::chrono::nanoseconds{0});
        }

        // Periodic timer, first fire one period from now. Loop thread only while running.
        template <typename Rep, typename Period>
        timer every(std::chrono::duration<Rep, Period> period) {
            const auto p = std::chrono::duration_cast<std::chrono::nanoseconds>(period);
            if (p.count() <= 0)
                throw std::invalid_argument("event_loop: period must be positive");
            return make_timer(p, p);
        }

        // Run a task on the loop thread. Safe from any thread; the one door in.
        void post(std::function<void()> task) {
            auto c = c_;
            c->enqueue_task(std::move(task));
        }

        // Run until stop(). One thread; a second concurrent run throws.
        void run() {
            auto keep = c_;                     // core outlives even a moved/destroyed handle
            run_guard guard(*keep);
            keep->stop_flag.store(false, std::memory_order_release);
            while (!keep->stop_flag.load(std::memory_order_acquire))
                keep->iterate(-1);
        }

        // One iteration: dispatch what is ready, waiting at most max_wait.
        // Returns whether anything was dispatched.
        bool run_once(std::chrono::milliseconds max_wait = std::chrono::milliseconds{0}) {
            auto keep = c_;
            run_guard guard(*keep);
            return keep->iterate(max_wait.count() < 0 ? 0 : max_wait.count());
        }

        // Safe from any thread
        void stop() {
            auto c = c_;
            c->stop_flag.store(true, std::memory_order_release);
            c->wake();
        }

        bool running() const noexcept {
            return c_ && c_->running.load(std::memory_order_acquire);
        }

        // The journal tap: every delivery (task, timer, fd event) is announced
        // here before it is dispatched - subscribe to record, re-drive handler
        // signals to replay
        moveable_signal<const dispatch_info&>& on_dispatch() { return c_->tap; }

    private:
        timer make_timer(std::chrono::nanoseconds delay, std::chrono::nanoseconds period) {
            auto c = c_;
            c->require_loop_thread("timer creation");
            auto st = std::make_shared<timer_state>();
            st->id = c->next_timer_id++;
            st->period = period;
            c->timers.push(heap_entry{std::chrono::steady_clock::now() + delay,
                                      c->next_seq++, st});
            return timer(std::move(st));
        }
    };

} // namespace snicholls

#endif // !defined(_WIN32)
#endif /* event_loop_hpp */
