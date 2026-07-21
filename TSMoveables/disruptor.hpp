//
//  disruptor.hpp
//  TSMoveables
//
//  Created by Saxon Nicholls on 21/7/2026.
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables
//
//  After: Thompson, Farley, Barker, Gee, Stewart -
//  "Disruptor: High performance alternative to bounded queues for exchanging
//  data between concurrent threads" (LMAX, 2011)
//

#ifndef disruptor_hpp
#define disruptor_hpp

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace snicholls
{
    namespace detail
    {
        inline constexpr std::size_t sequence_cache_line_size =
#if defined(__aarch64__) || defined(_M_ARM64)
            128;
#else
            64;
#endif

        // A padded published/processed counter. -1 means "nothing yet".
        struct alignas(sequence_cache_line_size) sequence {
            std::atomic<std::int64_t> v{-1};
        };
    } // namespace detail

    // Wait strategies - how a consumer with nothing to do waits for the
    // producer's cursor (or its dependency barrier) to advance. The strategy
    // is called in a loop: wait(ready) may return spuriously; the caller
    // re-checks. signal() is called by the producer after every publish.

    // Lowest latency, burns a core
    struct busy_spin_wait_strategy {
        template <typename Pred>
        void wait(Pred ready) {
            while (!ready()) {
            }
        }
        void signal() noexcept {}
    };

    // The default: spin briefly, then yield the timeslice
    struct yielding_wait_strategy {
        template <typename Pred>
        void wait(Pred ready) {
            for (int i = 0; i < 256; ++i)
                if (ready())
                    return;
            std::this_thread::yield();
        }
        void signal() noexcept {}
    };

    // Sleeps on a condition variable; kindest to the machine, microseconds of
    // wake latency. The 1ms timeout bounds the cost of a lost race between a
    // publish and a consumer going to sleep.
    struct blocking_wait_strategy {
        std::mutex m;
        std::condition_variable cv;
        template <typename Pred>
        void wait(Pred ready) {
            std::unique_lock<std::mutex> lock(m);
            cv.wait_for(lock, std::chrono::milliseconds(1), ready);
        }
        void signal() noexcept {
            cv.notify_all();
        }
    };

    // Disruptor - phase 1: single producer, any number of consumers, consumer
    // dependency graphs, batch consumption.
    //
    // A pre-allocated ring of default-constructed events. The producer claims
    // the next sequence, mutates the event in place, and publishes by
    // advancing the cursor - no allocation, no locking, one release store.
    // Consumers see contiguous batches and record progress in their own
    // padded sequence; a consumer constructed with dependencies will not see
    // an event until every dependency has processed it. The producer is gated
    // by the slowest consumer so it can never lap anyone.
    //
    // Wiring happens before the data flows: add all consumers, then publish
    // (add_consumer after the first publish throws std::logic_error). One
    // thread publishes; each consumer is pumped by one thread via poll() or
    // run(). Handlers receive T& - dependent stages may write fields for
    // stages downstream of them (the sequence protocol orders those writes);
    // consumers at the same barrier level must treat shared events as
    // read-only.
    //
    // Movability: every shared byte lives behind a stable heap core, so
    // moving the disruptor handle just transfers ownership of that core -
    // consumer references and running threads are unaffected. The moved-from
    // handle is empty; publishing continues through the new handle (from the
    // same thread, or via an externally synchronized handoff, as the single-
    // producer contract already requires).
    template <typename T, typename WaitStrategy = yielding_wait_strategy>
    class disruptor {

        static_assert(std::is_default_constructible_v<T>,
                      "disruptor: events are pre-allocated, so T must be default constructible");

        struct core;

    public:
        using value_type = T;
        using wait_strategy_type = WaitStrategy;

        class consumer {
            friend class disruptor;

            core* c_;
            detail::sequence seq_;                          // last processed
            std::vector<const detail::sequence*> barrier_;  // cursor, or dependency sequences

            // The lowest sequence this consumer may process up to
            std::int64_t available() const noexcept {
                std::int64_t m = c_->cursor.v.load(std::memory_order_acquire);
                for (const detail::sequence* s : barrier_) {
                    const std::int64_t d = s->v.load(std::memory_order_acquire);
                    if (d < m)
                        m = d;
                }
                return m;
            }

        public:
            // Constructed by add_consumer - core is private, so only the
            // disruptor can supply these arguments
            consumer(core* c, std::vector<const detail::sequence*> barrier) noexcept
                : c_(c), barrier_(std::move(barrier)) {}

            consumer(const consumer&) = delete;
            consumer& operator=(const consumer&) = delete;

            // Process every available event: f(T& event, int64_t seq, bool end_of_batch).
            // Returns how many were processed. If f throws, progress is not
            // recorded and the batch is redelivered on the next poll.
            template <typename F>
            std::size_t poll(F&& f) {
                const std::int64_t next = seq_.v.load(std::memory_order_relaxed) + 1;
                const std::int64_t avail = available();
                if (avail < next)
                    return 0;
                for (std::int64_t s = next; s <= avail; ++s)
                    f(c_->events[static_cast<std::size_t>(s) & c_->mask], s, s == avail);
                seq_.v.store(avail, std::memory_order_release);
                return static_cast<std::size_t>(avail - next + 1);
            }

            // Pump until keep_running is false, waiting through the disruptor's
            // wait strategy when idle. Events published before the flag drops
            // may remain unprocessed - drain with poll() afterwards if needed.
            template <typename F>
            void run(const std::atomic<bool>& keep_running, F&& f) {
                while (keep_running.load(std::memory_order_acquire)) {
                    if (poll(f) == 0)
                        c_->wait.wait([&] {
                            return !keep_running.load(std::memory_order_acquire) ||
                                   available() > seq_.v.load(std::memory_order_relaxed);
                        });
                }
            }

            std::int64_t last_processed() const noexcept {
                return seq_.v.load(std::memory_order_acquire);
            }
        };

    private:
        struct core {
            std::unique_ptr<T[]> events;
            std::size_t mask;
            detail::sequence cursor;                        // last published
            WaitStrategy wait{};
            std::deque<consumer> consumers;                 // deque: stable addresses
            std::vector<const detail::sequence*> gating;    // every consumer's sequence
            std::int64_t next = 0;                          // producer-owned: next to claim
            std::int64_t cached_gate = -1;                  // producer's stale view of the slowest consumer
            std::atomic<bool> started{false};

            explicit core(std::size_t capacity)
                : events(new T[capacity]), mask(capacity - 1) {}

            std::int64_t min_gating() const noexcept {
                if (gating.empty())
                    return next - 1;                        // nobody consumes: never gated
                std::int64_t m = gating.front()->v.load(std::memory_order_acquire);
                for (std::size_t i = 1; i < gating.size(); ++i) {
                    const std::int64_t s = gating[i]->v.load(std::memory_order_acquire);
                    if (s < m)
                        m = s;
                }
                return m;
            }
        };

        std::unique_ptr<core> c_;

        static constexpr std::size_t round_up_pow2(std::size_t n) noexcept {
            std::size_t p = 1;
            while (p < n)
                p <<= 1;
            return p;
        }

        // Producer gating: wait until publishing seq `last` cannot lap the
        // slowest consumer. Plain yield loop - producers gate rarely in a
        // well-sized ring, and it keeps the wait strategy consumer-only.
        void wait_for_room(core& c, std::int64_t last) {
            const std::int64_t wrap = last - static_cast<std::int64_t>(c.mask + 1);
            if (wrap > c.cached_gate) {
                c.cached_gate = c.min_gating();
                while (wrap > c.cached_gate) {
                    std::this_thread::yield();
                    c.cached_gate = c.min_gating();
                }
            }
        }

    public:
        explicit disruptor(std::size_t capacity) {
            if (capacity == 0)
                throw std::invalid_argument("disruptor: capacity must be positive");
            c_ = std::make_unique<core>(round_up_pow2(capacity));
        }

        disruptor(disruptor&&) noexcept = default;
        disruptor& operator=(disruptor&&) noexcept = default;
        disruptor(const disruptor&) = delete;
        disruptor& operator=(const disruptor&) = delete;

        // Wire a consumer. No dependencies: gated on the producer's cursor.
        // With dependencies: sees an event only after every dependency has
        // processed it. Must happen before the first publish.
        consumer& add_consumer(std::initializer_list<consumer*> deps = {}) {
            core& c = *c_;
            if (c.started.load(std::memory_order_acquire))
                throw std::logic_error("disruptor: consumers must be wired before publishing begins");
            std::vector<const detail::sequence*> barrier;
            for (consumer* d : deps)
                barrier.push_back(&d->seq_);
            c.consumers.emplace_back(&c, std::move(barrier));
            c.gating.push_back(&c.consumers.back().seq_);
            return c.consumers.back();
        }

        // Claim the next event, mutate it in place, publish it. Blocks
        // (yielding) while the ring is full. Single producer thread only.
        template <typename F>
        void publish(F&& fill) {
            core& c = *c_;
            c.started.store(true, std::memory_order_release);
            const std::int64_t next = c.next;
            wait_for_room(c, next);
            fill(c.events[static_cast<std::size_t>(next) & c.mask]);
            // Release: pairs with consumers' acquire of the cursor - the event
            // contents happen-before anyone processes the sequence
            c.cursor.v.store(next, std::memory_order_release);
            c.next = next + 1;
            c.wait.signal();
        }

        // As publish, but returns false instead of blocking when full
        template <typename F>
        bool try_publish(F&& fill) {
            core& c = *c_;
            c.started.store(true, std::memory_order_release);
            const std::int64_t next = c.next;
            const std::int64_t wrap = next - static_cast<std::int64_t>(c.mask + 1);
            if (wrap > c.cached_gate) {
                c.cached_gate = c.min_gating();
                if (wrap > c.cached_gate)
                    return false;
            }
            fill(c.events[static_cast<std::size_t>(next) & c.mask]);
            c.cursor.v.store(next, std::memory_order_release);
            c.next = next + 1;
            c.wait.signal();
            return true;
        }

        // Claim n consecutive events, fill each via fill(T&, int64_t seq),
        // publish them with one cursor advance and one signal
        template <typename F>
        void publish_n(std::size_t n, F&& fill) {
            core& c = *c_;
            if (n == 0)
                return;
            if (n > capacity())
                throw std::invalid_argument("disruptor: batch larger than capacity");
            c.started.store(true, std::memory_order_release);
            const std::int64_t first = c.next;
            const std::int64_t last = first + static_cast<std::int64_t>(n) - 1;
            wait_for_room(c, last);
            for (std::int64_t s = first; s <= last; ++s)
                fill(c.events[static_cast<std::size_t>(s) & c.mask], s);
            c.cursor.v.store(last, std::memory_order_release);
            c.next = last + 1;
            c.wait.signal();
        }

        std::int64_t last_published() const noexcept {
            return c_->cursor.v.load(std::memory_order_acquire);
        }

        std::size_t capacity() const noexcept { return c_->mask + 1; }
    };

} // namespace snicholls

#endif /* disruptor_hpp */
