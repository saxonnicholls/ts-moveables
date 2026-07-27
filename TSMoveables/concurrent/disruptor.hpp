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

    // Which producer discipline a disruptor is built for. This is a
    // compile-time choice rather than a runtime flag on purpose: the
    // single-producer path is the one people benchmark, and it must not pay a
    // branch, a CAS or an availability array for a mode it never uses.
    // Multi-producer users spell it out through multi_producer_disruptor.
    enum class producer_mode { single, multi };

    namespace detail
    {
        // Multi-producer bookkeeping. The primary template is empty, so a
        // single-producer disruptor allocates nothing here and carries none of
        // these members - the mode you did not choose costs you nothing.
        template <bool Multi>
        struct multi_producer_state {
            void init(std::size_t) noexcept {}
        };

        template <>
        struct multi_producer_state<true> {
            // A shared, deliberately stale view of the slowest consumer, so
            // producers do not have to scan every gating sequence on every
            // claim. Padded: it is written on the slow path but read on the
            // fast one by every producer.
            sequence gate_cache;

            // One mark per slot, holding the *round* on which the slot was
            // last published: round(s) = s >> index_bits. -1 means "never".
            //
            // Why the round and not a bool - this is the subtle part. With
            // several producers in flight, sequence N+1 can be published
            // before N, so the cursor alone no longer means "everything up to
            // here is readable" and each slot has to say for itself. A bool
            // would have to be cleared when the slot is recycled, and there is
            // no safe moment to clear it: the producer that claims the slot
            // for the next lap cannot clear before filling (a consumer would
            // then see an already-published event vanish) and cannot clear
            // after filling (until it does, the previous lap's `true` reads as
            // this lap's publication, and a consumer walks straight into a
            // half-written event). Storing the round makes the mark
            // self-describing instead: a mark written on lap k can never
            // answer a question about lap k+1, so nothing ever needs clearing
            // and wraparound cannot be mistaken for availability.
            //
            // Held as int64 so no truncation argument is needed - the round
            // has the same range as the sequence it came from.
            std::unique_ptr<std::atomic<std::int64_t>[]> avail;
            std::size_t mask = 0;
            int index_bits = 0;

            void init(std::size_t capacity) {
                mask = capacity - 1;
                index_bits = 0;
                while ((std::size_t{1} << index_bits) < capacity)
                    ++index_bits;
                avail = std::unique_ptr<std::atomic<std::int64_t>[]>(
                    new std::atomic<std::int64_t>[capacity]);
                for (std::size_t i = 0; i < capacity; ++i)
                    avail[i].store(-1, std::memory_order_relaxed);   // no lap published yet
            }

            bool is_available(std::int64_t s) const noexcept {
                // Acquire: pairs with the release store in publish(). Seeing
                // this lap's mark is exactly what makes the producer's writes
                // into the slot visible to the consumer about to read them.
                return avail[static_cast<std::size_t>(s) & mask].load(std::memory_order_acquire) ==
                       (s >> index_bits);
            }

            void publish(std::int64_t s) noexcept {
                // Release: everything this producer wrote into the slot
                // happens-before any consumer that observes the mark
                avail[static_cast<std::size_t>(s) & mask].store(s >> index_bits,
                                                                std::memory_order_release);
            }

            // The highest sequence in [from, upto] with no unpublished hole
            // below it. `from` must be a sequence already known to be
            // consumed or published - the walk cannot start in the middle of
            // nowhere, because a gap below it would go unnoticed.
            std::int64_t highest_published(std::int64_t from, std::int64_t upto) const noexcept {
                for (std::int64_t s = from; s <= upto; ++s)
                    if (!is_available(s))
                        return s - 1;
                return upto;
            }
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

    // Disruptor - any number of consumers, consumer dependency graphs, batch
    // consumption, and either producer discipline.
    //
    // A pre-allocated ring of default-constructed events. A producer claims a
    // sequence, mutates the event in place, and publishes it - no allocation,
    // no locking. Consumers see contiguous batches and record progress in
    // their own padded sequence; a consumer constructed with dependencies will
    // not see an event until every dependency has processed it. Producers are
    // gated by the slowest consumer so they can never lap anyone.
    //
    // Two producer disciplines, chosen at compile time:
    //
    //   disruptor<T>                    one producer thread (phase 1). The
    //                                   producer owns the claim counter
    //                                   outright and publishes with a single
    //                                   release store to the cursor, which
    //                                   therefore means "everything up to here
    //                                   is readable".
    //   multi_producer_disruptor<T>     any number of producer threads (phase
    //                                   2). Producers CAS sequences out of a
    //                                   shared claim counter, so sequence N+1
    //                                   can be published while N is still
    //                                   being written; publication is recorded
    //                                   per slot instead, and consumers walk
    //                                   those marks to find the highest
    //                                   *contiguously* published sequence.
    //
    // The multi-producer mode costs a CAS per claim, one availability array of
    // int64 per slot, and a short mark walk per poll. That is why it is opt-in
    // rather than the default: nothing in the single-producer path branches on
    // it or allocates for it.
    //
    // Named honestly: the claim loop is lock-free, not wait-free, and the
    // contiguity guarantee means a producer that claims a sequence and then
    // stalls holds up every consumer behind it (and eventually every other
    // producer, once the ring fills). That is inherent to the pattern - a
    // consumer that must see a dense, ordered stream cannot step over a hole -
    // and LMAX's implementation behaves the same way. Fill callbacks should be
    // short and must not block on anything the consumers are needed for.
    //
    // Wiring happens before the data flows: add all consumers, then publish
    // (add_consumer after the first publish throws std::logic_error). Each
    // consumer is pumped by one thread via poll() or run(). Handlers receive
    // T& - dependent stages may write fields for stages downstream of them
    // (the sequence protocol orders those writes); consumers at the same
    // barrier level must treat shared events as read-only.
    //
    // Movability: every shared byte lives behind a stable heap core, so
    // moving the disruptor handle just transfers ownership of that core -
    // consumer references and running threads are unaffected. The moved-from
    // handle is empty; publishing continues through the new handle. The handle
    // object itself is not a synchronisation point in either mode, so a move
    // needs the same externally synchronized handoff the single-producer
    // contract already asks for - the ring, the consumers and the in-flight
    // events do not care.
    template <typename T, typename WaitStrategy = yielding_wait_strategy,
              producer_mode Producers = producer_mode::single>
    class disruptor {

        static_assert(std::is_default_constructible_v<T>,
                      "disruptor: events are pre-allocated, so T must be default constructible");

        static constexpr bool multi = (Producers == producer_mode::multi);

        struct core;

    public:
        using value_type = T;
        using wait_strategy_type = WaitStrategy;

        class consumer {
            friend class disruptor;

            core* c_;
            detail::sequence seq_;                          // last processed
            std::vector<const detail::sequence*> barrier_;  // cursor, or dependency sequences

            // The highest sequence this consumer may process, given that it is
            // about to resume at `from` (only the multi-producer walk needs
            // the starting point)
            std::int64_t available(std::int64_t from) const noexcept {
                if constexpr (multi) {
                    if (!barrier_.empty()) {
                        // A dependency records only sequences it has already
                        // processed, and it could not have processed an
                        // unpublished event - so the minimum over the
                        // dependencies is already a contiguous bound and the
                        // availability marks would add nothing
                        std::int64_t m = barrier_.front()->v.load(std::memory_order_acquire);
                        for (std::size_t i = 1; i < barrier_.size(); ++i) {
                            const std::int64_t d = barrier_[i]->v.load(std::memory_order_acquire);
                            if (d < m)
                                m = d;
                        }
                        return m;
                    }
                    // Gated on the producers directly. Here the cursor is the
                    // highest sequence *claimed*, which with several producers
                    // in flight can be well past the highest one published, so
                    // it is only an upper bound for the walk. `from` is safe to
                    // start from: everything below it has already been
                    // processed by this consumer.
                    return c_->mp.highest_published(from,
                                                    c_->cursor.v.load(std::memory_order_acquire));
                } else {
                    (void)from;
                    std::int64_t m = c_->cursor.v.load(std::memory_order_acquire);
                    for (const detail::sequence* s : barrier_) {
                        const std::int64_t d = s->v.load(std::memory_order_acquire);
                        if (d < m)
                            m = d;
                    }
                    return m;
                }
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
                const std::int64_t avail = available(next);
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
                            const std::int64_t last = seq_.v.load(std::memory_order_relaxed);
                            return !keep_running.load(std::memory_order_acquire) ||
                                   available(last + 1) > last;
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
            // Single-producer: the last sequence *published*, and the whole of
            // the consumers' barrier. Multi-producer: the last sequence
            // *claimed* - publication is recorded per slot in `mp` instead.
            // LMAX splits its single- and multi-producer sequencers along
            // exactly this line.
            detail::sequence cursor;
            WaitStrategy wait{};
            std::deque<consumer> consumers;                 // deque: stable addresses
            std::vector<const detail::sequence*> gating;    // every consumer's sequence
            std::int64_t next = 0;                          // single-producer: next to claim
            std::int64_t cached_gate = -1;                  // single-producer: stale view of the slowest consumer
            std::atomic<bool> started{false};
            // Empty (and allocation-free) unless Producers == multi. Placed
            // last so the single-producer layout above is untouched.
            detail::multi_producer_state<multi> mp;

            explicit core(std::size_t capacity)
                : events(new T[capacity]), mask(capacity - 1) {
                mp.init(capacity);
            }

            // Lowest sequence any consumer has processed, or `if_empty` when
            // nothing consumes - callers pass a value that cannot gate them
            std::int64_t min_gating(std::int64_t if_empty) const noexcept {
                if (gating.empty())
                    return if_empty;
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

        static void mark_started(core& c) noexcept {
            if constexpr (multi) {
                // Several producers writing the same shared line on every
                // publish would be pure contention for a flag that only ever
                // goes false -> true. A relaxed load leaves the line shared.
                if (!c.started.load(std::memory_order_relaxed))
                    c.started.store(true, std::memory_order_release);
            } else {
                c.started.store(true, std::memory_order_release);
            }
        }

        // Producer gating: wait until publishing seq `last` cannot lap the
        // slowest consumer. Plain yield loop - producers gate rarely in a
        // well-sized ring, and it keeps the wait strategy consumer-only.
        void wait_for_room(core& c, std::int64_t last) {
            const std::int64_t wrap = last - static_cast<std::int64_t>(c.mask + 1);
            if (wrap > c.cached_gate) {
                c.cached_gate = c.min_gating(c.next - 1);
                while (wrap > c.cached_gate) {
                    std::this_thread::yield();
                    c.cached_gate = c.min_gating(c.next - 1);
                }
            }
        }

        // Multi-producer claim: take n consecutive sequences out of the shared
        // cursor with a CAS. The winner owns those slots outright until it
        // marks them published, so the events themselves need no further
        // synchronisation between producers. Gating happens before the CAS: a
        // claim that would lap a slot some consumer has not finished with is
        // never made. Returns the last sequence claimed through `last_out`;
        // with Blocking == false, returns false instead of waiting when full.
        template <bool Blocking>
        bool claim_multi(core& c, std::int64_t n, std::int64_t& last_out) {
            const std::int64_t cap = static_cast<std::int64_t>(c.mask) + 1;
            std::int64_t current = c.cursor.v.load(std::memory_order_relaxed);
            for (;;) {
                const std::int64_t last = current + n;
                const std::int64_t wrap = last - cap;
                // Acquire: pairs with the release store below. The cache is
                // only ever written from a real scan of the gating sequences,
                // and those only ever move forward, so a cached value can
                // never exceed the true minimum. Trusting it can therefore
                // only make a producer scan when it need not have - never let
                // one through when it should have waited.
                if (wrap > c.mp.gate_cache.v.load(std::memory_order_acquire)) {
                    const std::int64_t gate = c.min_gating(last);   // `last` never gates
                    if (wrap > gate) {
                        if constexpr (!Blocking)
                            return false;                          // ring is full
                        std::this_thread::yield();
                        current = c.cursor.v.load(std::memory_order_relaxed);
                        continue;
                    }
                    // Release: the acquire loads inside min_gating are what
                    // order each consumer's reads of these slots before this
                    // producer's writes to them. A producer that later takes
                    // the fast path on this cached value inherits that
                    // ordering only if the store is a release and its load an
                    // acquire - happens-before is transitive through the pair,
                    // and without it that producer would have established
                    // nothing with the consumers at all.
                    c.mp.gate_cache.v.store(gate, std::memory_order_release);
                }
                // Relaxed on both success and failure: the cursor hands out
                // disjoint sequence numbers and carries no data of its own.
                // Modification order on a single atomic is total regardless of
                // ordering, which is all a claim needs. What orders the slot
                // contents is the gating above (before we write) and the
                // availability mark (after we write). A failed CAS has already
                // refreshed `current`, so we simply go round again.
                if (c.cursor.v.compare_exchange_weak(current, last, std::memory_order_relaxed,
                                                     std::memory_order_relaxed)) {
                    last_out = last;
                    return true;
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
        // (yielding) while the ring is full. Single-producer mode: one
        // producer thread only. Multi-producer mode: callable from any number
        // of threads at once.
        template <typename F>
        void publish(F&& fill) {
            core& c = *c_;
            mark_started(c);
            if constexpr (multi) {
                std::int64_t seq = 0;
                (void)claim_multi<true>(c, 1, seq);
                fill(c.events[static_cast<std::size_t>(seq) & c.mask]);
                c.mp.publish(seq);
                c.wait.signal();
            } else {
                const std::int64_t next = c.next;
                wait_for_room(c, next);
                fill(c.events[static_cast<std::size_t>(next) & c.mask]);
                // Release: pairs with consumers' acquire of the cursor - the event
                // contents happen-before anyone processes the sequence
                c.cursor.v.store(next, std::memory_order_release);
                c.next = next + 1;
                c.wait.signal();
            }
        }

        // As publish, but returns false instead of blocking when full
        template <typename F>
        bool try_publish(F&& fill) {
            core& c = *c_;
            mark_started(c);
            if constexpr (multi) {
                std::int64_t seq = 0;
                if (!claim_multi<false>(c, 1, seq))
                    return false;
                fill(c.events[static_cast<std::size_t>(seq) & c.mask]);
                c.mp.publish(seq);
                c.wait.signal();
                return true;
            } else {
                const std::int64_t next = c.next;
                const std::int64_t wrap = next - static_cast<std::int64_t>(c.mask + 1);
                if (wrap > c.cached_gate) {
                    c.cached_gate = c.min_gating(c.next - 1);
                    if (wrap > c.cached_gate)
                        return false;
                }
                fill(c.events[static_cast<std::size_t>(next) & c.mask]);
                c.cursor.v.store(next, std::memory_order_release);
                c.next = next + 1;
                c.wait.signal();
                return true;
            }
        }

        // Claim n consecutive events, fill each via fill(T&, int64_t seq),
        // publish them with one signal. Single-producer mode advances the
        // cursor once; multi-producer mode marks each slot, because
        // publication there is per slot by construction.
        template <typename F>
        void publish_n(std::size_t n, F&& fill) {
            core& c = *c_;
            if (n == 0)
                return;
            if (n > capacity())
                throw std::invalid_argument("disruptor: batch larger than capacity");
            mark_started(c);
            if constexpr (multi) {
                std::int64_t last = 0;
                (void)claim_multi<true>(c, static_cast<std::int64_t>(n), last);
                const std::int64_t first = last - static_cast<std::int64_t>(n) + 1;
                for (std::int64_t s = first; s <= last; ++s)
                    fill(c.events[static_cast<std::size_t>(s) & c.mask], s);
                // Mark ascending. Correctness does not depend on the order -
                // every mark is a release store that carries all of this
                // producer's writes so far - but ascending is what lets a
                // consumer pick up the front of the batch without waiting for
                // the tail, instead of stalling on a hole we are about to fill.
                for (std::int64_t s = first; s <= last; ++s)
                    c.mp.publish(s);
                c.wait.signal();
            } else {
                const std::int64_t first = c.next;
                const std::int64_t last = first + static_cast<std::int64_t>(n) - 1;
                wait_for_room(c, last);
                for (std::int64_t s = first; s <= last; ++s)
                    fill(c.events[static_cast<std::size_t>(s) & c.mask], s);
                c.cursor.v.store(last, std::memory_order_release);
                c.next = last + 1;
                c.wait.signal();
            }
        }

        // The highest sequence every consumer may safely look at. Single
        // producer: one cursor read. Multi-producer: a walk of the
        // availability marks, because no single counter can mean
        // "contiguously published" there - that is precisely why the marks
        // exist. Treat the multi-producer form as a query, not as part of the
        // publish/consume protocol; consumers use their own barrier.
        std::int64_t last_published() const noexcept {
            core& c = *c_;
            if constexpr (multi) {
                const std::int64_t claimed = c.cursor.v.load(std::memory_order_acquire);
                // Where the walk may start. Nothing at or below the slowest
                // consumer can still be unpublished - it was consumed - and
                // with no consumers at all nothing further back than one lap
                // survives anyway, so the older marks are not worth reading.
                std::int64_t from = claimed - static_cast<std::int64_t>(c.mask);
                const std::int64_t gate = c.min_gating(-1);
                if (gate + 1 > from)
                    from = gate + 1;
                if (from < 0)
                    from = 0;
                return c.mp.highest_published(from, claimed);
            } else {
                return c.cursor.v.load(std::memory_order_acquire);
            }
        }

        std::size_t capacity() const noexcept { return c_->mask + 1; }
    };

    // The opt-in multi-producer disruptor: same ring, same consumers, same
    // dependency graphs; producers CAS their sequences out of a shared claim
    // counter and publish through per-slot availability marks.
    template <typename T, typename WaitStrategy = yielding_wait_strategy>
    using multi_producer_disruptor = disruptor<T, WaitStrategy, producer_mode::multi>;

} // namespace snicholls

#endif /* disruptor_hpp */
