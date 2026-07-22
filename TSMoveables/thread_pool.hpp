//
//  thread_pool.hpp
//  TSMoveables
//
//  Created by Saxon Nicholls on 22/7/2026.
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables
//

#ifndef thread_pool_hpp
#define thread_pool_hpp

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "moveable_condition_variable.hpp"
#include "synchronized.hpp"
#include "circular_buffer.hpp"
#include "mpmc_queue.hpp"
#include "work_stealing_deque.hpp"

namespace snicholls
{
    // Thread pool - a pure-virtual interface, and implementations spanning the
    // design space so they can be compared against one workload.
    //
    // The interface is the point as much as any single pool: the interesting
    // choices (one shared queue vs. sharded vs. lock-free hand-off; block vs.
    // spin) are hard to compare fairly without a common surface. Every
    // implementation here is built from this library's own primitives.
    //
    // Cost note: the virtual submit(std::function<void()>) path pays a
    // type-erasure and a virtual call per task - the overhead the rest of the
    // library avoids. Use it for comparison and polymorphic wiring; a latency-
    // critical caller takes a concrete pool by value and a concrete callable.
    //
    // We do not try to beat the work-stealing greats (Taskflow, TBB, Tokio).
    // What we add: pools composed visibly from these primitives, a moveable
    // pool handle, dependency-free/header-only/C++17, and a level harness.
    struct task_pool {
        using task = std::function<void()>;

        virtual ~task_pool() = default;

        virtual void submit(task t) = 0;                    // enqueue work
        virtual void wait_idle() = 0;                       // block until all submitted work has run
        virtual std::size_t worker_count() const noexcept = 0;
    };

    // Result-returning submission over ANY implementation - non-virtual and
    // generic, like snicholls::call_once. Wraps a packaged_task, returns its
    // future; the shared_ptr keeps the task alive until the pool runs it.
    template <typename F>
    auto async(task_pool& pool, F&& f) -> std::future<std::invoke_result_t<std::decay_t<F>>>
    {
        using result_type = std::invoke_result_t<std::decay_t<F>>;
        auto pt = std::make_shared<std::packaged_task<result_type()>>(std::forward<F>(f));
        std::future<result_type> fut = pt->get_future();
        pool.submit([pt] { (*pt)(); });
        return fut;
    }

    namespace detail
    {
        inline std::size_t default_workers() noexcept
        {
            const unsigned h = std::thread::hardware_concurrency();
            return h != 0 ? h : 4;
        }

        // Outstanding-work tracker for wait_idle. The count is atomic so the
        // hot submit/complete path never takes the lock; the mutex is touched
        // only to publish the "reached zero" wakeup and by waiters.
        struct completion {
            std::mutex m;
            moveable_condition_variable<> cv;
            std::atomic<long long> outstanding{0};

            void add() noexcept { outstanding.fetch_add(1, std::memory_order_relaxed); }

            void done() {
                if (outstanding.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    std::lock_guard<std::mutex> lock(m);
                    cv.notify_all();
                }
            }

            void wait() {
                std::unique_lock<std::mutex> lock(m);
                cv.wait(lock, [this] { return outstanding.load(std::memory_order_acquire) == 0; });
            }
        };

        using pool_task = task_pool::task;
        using task_queue = std::deque<pool_task>;

        // Pop one task under the lock; returns whether it ran this worker's turn.
        // Shared by the two condition-variable-backed pools.
        inline bool drain_one(task_queue& q, pool_task& out) {
            if (q.empty())
                return false;
            out = std::move(q.front());
            q.pop_front();
            return true;
        }
    } // namespace detail

    // One shared queue, workers blocked on its condition variable. The honest
    // "correct but contended" reference every faster design must beat.
    // submit may be called from any thread.
    class mutex_task_pool final : public task_pool {

        struct core {
            detail::completion comp;
            std::atomic<bool> stopping{false};
            synchronized_waitable<detail::task_queue> q;
            std::vector<std::thread> workers;

            explicit core(std::size_t n) {
                workers.reserve(n);
                for (std::size_t i = 0; i < n; ++i)
                    workers.emplace_back([this] { run(); });
            }

            ~core() {
                stopping.store(true, std::memory_order_release);
                // Wake through a LOCKED notify: a bare notify_all can race a
                // worker that has read stopping==false but not yet enrolled in
                // the wait, and be lost (deadlocking join). update() takes the
                // queue mutex, which cannot be acquired until the worker has
                // enrolled, so the notify always reaches it.
                q.update([](detail::task_queue&) {});
                for (auto& w : workers)
                    w.join();
            }

            void run() {
                for (;;) {
                    detail::pool_task t;
                    const bool got = q.wait_then(
                        [this](const detail::task_queue& d) {
                            return stopping.load(std::memory_order_acquire) || !d.empty();
                        },
                        [&t](detail::task_queue& d) { return detail::drain_one(d, t); });
                    if (!got)
                        break;                          // stopping and drained
                    t();
                    comp.done();
                }
            }
        };

        std::unique_ptr<core> c_;

    public:
        explicit mutex_task_pool(std::size_t workers = detail::default_workers())
            : c_(std::make_unique<core>(workers != 0 ? workers : 1)) {}

        mutex_task_pool(mutex_task_pool&&) noexcept = default;
        mutex_task_pool& operator=(mutex_task_pool&&) noexcept = default;

        void submit(task t) override {
            c_->comp.add();
            c_->q.update([&t](detail::task_queue& d) { d.push_back(std::move(t)); });
        }
        void wait_idle() override { c_->comp.wait(); }
        std::size_t worker_count() const noexcept override { return c_->workers.size(); }
    };

    // K independent shards, one worker each, round-robin submission. Trades one
    // hot lock for K cool ones - the cheapest real win over the baseline.
    // submit may be called from any thread.
    class sharded_task_pool final : public task_pool {

        struct core {
            detail::completion comp;
            std::atomic<bool> stopping{false};
            std::atomic<std::size_t> rr{0};
            std::deque<synchronized_waitable<detail::task_queue>> shards;   // stable addresses
            std::vector<std::thread> workers;

            explicit core(std::size_t n) {
                for (std::size_t i = 0; i < n; ++i)
                    shards.emplace_back();
                workers.reserve(n);
                for (std::size_t i = 0; i < n; ++i)
                    workers.emplace_back([this, i] { run(i); });
            }

            ~core() {
                stopping.store(true, std::memory_order_release);
                // Locked notify, not a bare notify_all - see mutex_task_pool::core
                for (auto& s : shards)
                    s.update([](detail::task_queue&) {});
                for (auto& w : workers)
                    w.join();
            }

            void run(std::size_t i) {
                auto& shard = shards[i];
                for (;;) {
                    detail::pool_task t;
                    const bool got = shard.wait_then(
                        [this](const detail::task_queue& d) {
                            return stopping.load(std::memory_order_acquire) || !d.empty();
                        },
                        [&t](detail::task_queue& d) { return detail::drain_one(d, t); });
                    if (!got)
                        break;
                    t();
                    comp.done();
                }
            }
        };

        std::unique_ptr<core> c_;

    public:
        explicit sharded_task_pool(std::size_t workers = detail::default_workers())
            : c_(std::make_unique<core>(workers != 0 ? workers : 1)) {}

        sharded_task_pool(sharded_task_pool&&) noexcept = default;
        sharded_task_pool& operator=(sharded_task_pool&&) noexcept = default;

        void submit(task t) override {
            const std::size_t i = c_->rr.fetch_add(1, std::memory_order_relaxed) % c_->shards.size();
            c_->comp.add();
            c_->shards[i].update([&t](detail::task_queue& d) { d.push_back(std::move(t)); });
        }
        void wait_idle() override { c_->comp.wait(); }
        std::size_t worker_count() const noexcept override { return c_->workers.size(); }
    };

    // One lock-free SPSC circular_buffer per worker, hand-off with no lock at
    // all - the ring showcase. Because the ring is SPSC, this pool has a
    // SINGLE-DISPATCHER contract: submit() must be called from one thread. That
    // is the deterministic feed-handler pattern (one thread fans a feed out to
    // a worker per partition), and naming the constraint beats a "general" pool
    // that quietly serialises submission behind a lock.
    class dispatch_task_pool final : public task_pool {

        struct core {
            detail::completion comp;
            std::atomic<bool> stopping{false};
            std::deque<circular_buffer<detail::pool_task>> rings;   // stable addresses
            std::vector<std::thread> workers;
            std::size_t rr = 0;                                     // dispatcher-owned; single thread

            core(std::size_t n, std::size_t capacity) {
                for (std::size_t i = 0; i < n; ++i)
                    rings.emplace_back(capacity);
                workers.reserve(n);
                for (std::size_t i = 0; i < n; ++i)
                    workers.emplace_back([this, i] { run(i); });
            }

            ~core() {
                stopping.store(true, std::memory_order_release);
                for (auto& w : workers)
                    w.join();
            }

            void run(std::size_t i) {
                auto& ring = rings[i];
                detail::pool_task t;
                for (;;) {
                    if (ring.try_pop(t)) {
                        t();
                        comp.done();
                        continue;
                    }
                    if (stopping.load(std::memory_order_acquire)) {
                        while (ring.try_pop(t)) {           // drain before exit
                            t();
                            comp.done();
                        }
                        break;
                    }
                    std::this_thread::yield();
                }
            }
        };

        std::unique_ptr<core> c_;

    public:
        explicit dispatch_task_pool(std::size_t workers = detail::default_workers(),
                                    std::size_t per_worker_capacity = 1024)
            : c_(std::make_unique<core>(workers != 0 ? workers : 1,
                                        per_worker_capacity != 0 ? per_worker_capacity : 1024)) {}

        dispatch_task_pool(dispatch_task_pool&&) noexcept = default;
        dispatch_task_pool& operator=(dispatch_task_pool&&) noexcept = default;

        // Single-dispatcher contract: call from one thread only. Applies
        // backpressure (yields) when the target worker's ring is full.
        void submit(task t) override {
            c_->comp.add();
            const std::size_t i = c_->rr++;
            if (c_->rr >= c_->rings.size())
                c_->rr = 0;
            while (!c_->rings[i].try_push(std::move(t)))
                std::this_thread::yield();
        }
        void wait_idle() override { c_->comp.wait(); }
        std::size_t worker_count() const noexcept override { return c_->workers.size(); }
    };

    // One shared bounded MPMC queue, workers busy-poll it. General
    // submit-from-anywhere - the pool the single-dispatcher one could not be.
    // Workers spin (yield when idle) rather than parking, so there is no
    // condition-variable wakeup latency and no lost-wakeup surface; the cost is
    // CPU burned while idle, which a production build would replace with an
    // eventcount. Shutdown drains via wait_idle first, then flags and joins.
    class mpmc_task_pool final : public task_pool {

        struct core {
            detail::completion comp;
            std::atomic<bool> stopping{false};
            mpmc_queue<detail::pool_task> q;
            std::vector<std::thread> workers;

            core(std::size_t n, std::size_t capacity) : q(capacity) {
                workers.reserve(n);
                for (std::size_t i = 0; i < n; ++i)
                    workers.emplace_back([this] { run(); });
            }

            ~core() {
                comp.wait();                        // drain outstanding work
                stopping.store(true, std::memory_order_release);
                for (auto& w : workers)
                    w.join();
            }

            void run() {
                detail::pool_task t;
                for (;;) {
                    if (q.try_pop(t)) {
                        t();
                        comp.done();
                        continue;
                    }
                    if (stopping.load(std::memory_order_acquire))
                        break;
                    std::this_thread::yield();
                }
            }
        };

        std::unique_ptr<core> c_;

    public:
        explicit mpmc_task_pool(std::size_t workers = detail::default_workers(),
                                std::size_t capacity = 8192)
            : c_(std::make_unique<core>(workers != 0 ? workers : 1,
                                        capacity != 0 ? capacity : 8192)) {}

        mpmc_task_pool(mpmc_task_pool&&) noexcept = default;
        mpmc_task_pool& operator=(mpmc_task_pool&&) noexcept = default;

        void submit(task t) override {
            c_->comp.add();
            while (!c_->q.push(std::move(t)))       // backpressure when full
                std::this_thread::yield();
        }
        void wait_idle() override { c_->comp.wait(); }
        std::size_t worker_count() const noexcept override { return c_->workers.size(); }
    };

    namespace detail
    {
        // Identifies the worker (if any) the calling thread is running as, so a
        // task that submits more work pushes to its own local deque. Keyed by
        // core address so distinct pools don't confuse each other.
        struct ws_context {
            const void* pool = nullptr;
            std::size_t index = 0;
        };
        inline thread_local ws_context ws_current;
    } // namespace detail

    // Work-stealing pool - the design that competes with the incumbents on load
    // balance. Each worker owns a Chase-Lev deque (LIFO for the owner, FIFO for
    // thieves); a shared MPMC injector absorbs external submissions. A task
    // that submits more work while running pushes to its own local deque (hot,
    // cache-friendly); when a worker runs dry it drains the injector, then
    // steals from a random victim. Uses mpmc_queue and work_stealing_deque
    // together - the two components this line was gated on.
    //
    // Tasks are heap-allocated nodes so the deque moves only pointers, keeping
    // the single-element steal race free of the pointee. Workers busy-poll (no
    // parking), same tradeoff as mpmc_task_pool.
    class work_stealing_task_pool final : public task_pool {

        using node = task;                          // heap-allocated std::function

        struct core {
            detail::completion comp;
            std::atomic<bool> stopping{false};
            mpmc_queue<node*> injector;
            std::deque<work_stealing_deque<node>> locals;   // stable addresses
            std::vector<std::thread> workers;
            std::size_t n;

            core(std::size_t n_, std::size_t local_capacity, std::size_t inject_capacity)
                : injector(inject_capacity), n(n_) {
                for (std::size_t i = 0; i < n_; ++i)
                    locals.emplace_back(local_capacity);
                workers.reserve(n_);
                for (std::size_t i = 0; i < n_; ++i)
                    workers.emplace_back([this, i] { run(i); });
            }

            ~core() {
                comp.wait();                        // drain all outstanding work
                stopping.store(true, std::memory_order_release);
                for (auto& w : workers)
                    w.join();
            }

            void enqueue(node* p) {
                const detail::ws_context ctx = detail::ws_current;
                if (ctx.pool == this && locals[ctx.index].push(p))
                    return;                         // fast path: worker pushes its own deque
                if (injector.push(p))               // one non-blocking attempt
                    return;
                // Saturated - both the local deque and the injector are full.
                // Blocking here would deadlock a fork-join: every worker could
                // be stuck submitting while none is free to drain. Run it inline
                // instead - deadlock-free backpressure (the "caller runs" policy).
                (*p)();
                delete p;
                comp.done();
            }

            node* find_work(std::size_t self, std::uint64_t& rng) {
                if (node* p = locals[self].pop())
                    return p;
                node* ip = nullptr;
                if (injector.try_pop(ip))
                    return ip;
                for (std::size_t attempt = 0; attempt < n; ++attempt) {
                    rng ^= rng << 13;
                    rng ^= rng >> 7;
                    rng ^= rng << 17;               // xorshift64
                    std::size_t v = self;
                    if (n > 1) {
                        v = static_cast<std::size_t>(rng % n);
                        if (v == self)
                            v = (v + 1) % n;
                    }
                    bool lost = false;
                    if (node* s = locals[v].steal(lost))
                        return s;
                    // lost == true means a contended slot; keep trying victims
                }
                return nullptr;
            }

            void run(std::size_t self) {
                detail::ws_current = detail::ws_context{this, self};
                std::uint64_t rng = self * 0x9E3779B97F4A7C15ull + 1;
                for (;;) {
                    if (node* p = find_work(self, rng)) {
                        (*p)();
                        delete p;
                        comp.done();
                        continue;
                    }
                    if (stopping.load(std::memory_order_acquire))
                        break;
                    std::this_thread::yield();
                }
                detail::ws_current = detail::ws_context{};
            }
        };

        std::unique_ptr<core> c_;

    public:
        explicit work_stealing_task_pool(std::size_t workers = detail::default_workers(),
                                         std::size_t local_capacity = 1024,
                                         std::size_t inject_capacity = 8192)
            : c_(std::make_unique<core>(workers != 0 ? workers : 1,
                                        local_capacity != 0 ? local_capacity : 1024,
                                        inject_capacity != 0 ? inject_capacity : 8192)) {}

        work_stealing_task_pool(work_stealing_task_pool&&) noexcept = default;
        work_stealing_task_pool& operator=(work_stealing_task_pool&&) noexcept = default;

        void submit(task t) override {
            c_->comp.add();
            c_->enqueue(new node(std::move(t)));
        }
        void wait_idle() override { c_->comp.wait(); }
        std::size_t worker_count() const noexcept override { return c_->n; }
    };

} // namespace snicholls

#endif /* thread_pool_hpp */
