//
//  task_group.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - fork/join over any task_pool
//
//      snicholls::task_group g(pool);
//      g.run([&] { left  = solve(a); });
//      g.run([&] { right = solve(b); });
//      g.wait();                          // both done, or the first throw rethrown here
//
//  The pools answer "run this somewhere, I will find out later". This answers
//  "run these, and tell me when they are ALL done" - which is the shape almost
//  every divide-and-conquer algorithm actually wants, and the reason
//  parallel_for, parallel_reduce, parallel_scan and parallel_sort are all built
//  on the two pieces in here rather than on four copies of them.
//
//  ---------------------------------------------------------------- the design
//
//  Three decisions, and the third is the one that matters.
//
//  1. Exceptions are collected, not lost. A task that throws does not take the
//     process down and does not vanish: the first exception is kept and
//     rethrown from wait(), after every other task has finished. Later
//     exceptions are dropped, because they are usually consequences of the
//     first and a caller can only catch one anyway.
//
//  2. wait() is not optional, so the destructor does it. Every task captures
//     references to the caller's frame; returning from that frame while tasks
//     still run is a dangling reference, not a race you might get away with. A
//     destructor that waits turns forgetting into a stall you can see in a
//     stack trace instead of corruption you cannot.
//
//  3. THE WAITING THREAD RUNS TASKS TOO. This is what makes the group safe to
//     nest, and it is not an optimisation. A task_group waited on from inside a
//     pool task occupies a worker while it blocks; if every worker did that,
//     the pool would have no thread left to run the tasks they are all waiting
//     for, and the program would stop. So run() keeps the closure in the group
//     as well as offering it to the pool, and wait() executes whatever has not
//     been started yet before it blocks. A nested group therefore degrades to
//     serial rather than deadlocking - the same contract parallel_for makes,
//     for the same reason.
//
//  What this is NOT: a scheduler, and not a place to park blocking work. A task
//  that sleeps or waits on I/O occupies a pool thread for the duration; the
//  pool has no way to know it should start another. Submit compute here and
//  keep blocking calls on their own threads.
//

#ifndef task_group_hpp
#define task_group_hpp

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

#include "../interfaces/task_pool.hpp"

namespace snicholls
{
    namespace detail
    {
        // Outstanding-work tracker shared by task_group and the parallel
        // algorithms. The hot path (one unit finishing) touches only the
        // atomic; the mutex is taken solely to publish the reached-zero
        // wakeup.
        //
        // done() takes the lock BEFORE notifying rather than merely after
        // decrementing, and that ordering is the whole correctness argument: a
        // waiter that has evaluated its predicate but not yet slept would
        // otherwise miss the notify and park forever on work that is already
        // finished.
        class completion_gate
        {
        public:
            void add(std::size_t n) noexcept
            {
                outstanding_.fetch_add(n, std::memory_order_relaxed);
            }

            void done() noexcept
            {
                if (outstanding_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    std::lock_guard<std::mutex> g(m_);
                    cv_.notify_all();
                }
            }

            void wait()
            {
                std::unique_lock<std::mutex> lock(m_);
                cv_.wait(lock, [this] { return outstanding_.load(std::memory_order_acquire) == 0; });
            }

            std::size_t outstanding() const noexcept
            {
                return outstanding_.load(std::memory_order_acquire);
            }

        private:
            std::atomic<std::size_t> outstanding_{0};
            std::mutex               m_;
            std::condition_variable  cv_;
        };

        // First-exception-wins slot. `failed` is read on the hot path to stop
        // starting new work, so it is an atomic rather than a lock.
        class error_slot
        {
        public:
            void capture(std::exception_ptr e)
            {
                std::lock_guard<std::mutex> g(m_);
                if (!error_) {
                    error_ = e;
                    failed_.store(true, std::memory_order_release);
                }
            }

            bool failed() const noexcept { return failed_.load(std::memory_order_acquire); }

            void rethrow_if_failed() const
            {
                if (error_)
                    std::rethrow_exception(error_);
            }

        private:
            std::atomic<bool>  failed_{false};
            mutable std::mutex m_;
            std::exception_ptr error_;
        };
    } // namespace detail

    // Fork/join over any task_pool. Not moveable and not copyable on purpose:
    // outstanding tasks hold a pointer to the group's shared state, and a group
    // is a scope-bound thing like a lock_guard - moving one is not a use case,
    // it is a bug that would compile.
    class task_group
    {
    public:
        explicit task_group(task_pool& pool)
            : pool_(&pool), s_(std::make_shared<state>()) {}

        task_group(const task_group&) = delete;
        task_group& operator=(const task_group&) = delete;
        task_group(task_group&&) = delete;
        task_group& operator=(task_group&&) = delete;

        // Waiting is mandatory, so forgetting it stalls rather than corrupts -
        // see decision 2 in the header. An exception still in the slot at
        // destruction is swallowed: throwing from a destructor during stack
        // unwinding calls std::terminate, which would replace a diagnosable
        // problem with an undiagnosable one.
        ~task_group()
        {
            try {
                drain_pending();
                s_->gate.wait();
            } catch (...) {
            }
        }

        // Offer f to the pool and keep it here as well, so wait() can run it if
        // no worker got to it. Exactly one of the two ever executes it - the
        // claim flag decides, and the loser does nothing.
        template <typename F>
        void run(F&& f)
        {
            auto item = std::make_shared<unit>(std::function<void()>(std::forward<F>(f)));
            s_->gate.add(1);
            {
                std::lock_guard<std::mutex> g(s_->m);
                s_->pending.push_back(item);
            }
            auto s = s_;
            pool_->submit([s, item] { run_unit(*s, item); });
        }

        // Run whatever has not been claimed yet on THIS thread, then block
        // until the tasks other threads claimed have finished. Rethrows the
        // first exception any task threw.
        void wait()
        {
            drain_pending();
            s_->gate.wait();
            s_->err.rethrow_if_failed();
        }

        // Has any task thrown so far? Useful to abandon work early in a
        // long-running producer; wait() is still what reports the exception.
        bool failed() const noexcept { return s_->err.failed(); }

    private:
        struct unit {
            std::function<void()> fn;
            std::atomic<bool>     claimed{false};
            explicit unit(std::function<void()> f) : fn(std::move(f)) {}
        };

        struct state {
            detail::completion_gate           gate;
            detail::error_slot                err;
            std::mutex                        m;
            std::deque<std::shared_ptr<unit>> pending;
        };

        static void run_unit(state& s, const std::shared_ptr<unit>& item)
        {
            // Whoever flips the flag owns the call. The other side returns
            // without touching fn, which is what lets the pool task and the
            // waiting thread both go looking without a task ever running twice.
            if (item->claimed.exchange(true, std::memory_order_acq_rel))
                return;
            if (!s.err.failed()) {
                try {
                    item->fn();
                } catch (...) {
                    s.err.capture(std::current_exception());
                }
            }
            s.gate.done();
        }

        void drain_pending()
        {
            for (;;) {
                std::shared_ptr<unit> item;
                {
                    std::lock_guard<std::mutex> g(s_->m);
                    if (s_->pending.empty())
                        return;
                    item = std::move(s_->pending.front());
                    s_->pending.pop_front();
                }
                run_unit(*s_, item);
            }
        }

        task_pool*             pool_;
        std::shared_ptr<state> s_;
    };
} // namespace snicholls

#endif /* task_group_hpp */
