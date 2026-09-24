//
//  parallel_for.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - data-parallel loops over any task_pool
//
//  The pools in thread_pool.hpp answer "run this somewhere". This answers the
//  question actually asked most of the time: "run this loop across the cores I
//  have, and come back when it is done." It is a thin layer and deliberately
//  so - it adds no threads, owns no scheduler, and holds no global state. It
//  binds to the `task_pool` INTERFACE, so the same call runs on the shared-queue
//  pool, the sharded one, the MPMC one or the work-stealing one, and a
//  benchmark can swap between them without touching the loop.
//
//      snicholls::work_stealing_task_pool pool;
//      snicholls::parallel_for(pool, 0, n, [&](int i) { out[i] = f(in[i]); });
//      snicholls::parallel_for_each(pool, v.begin(), v.end(), [](auto& x) { x *= 2; });
//
//  Both block until every element has been processed, and both propagate the
//  first exception a body threw once the rest have settled.
//
//  ------------------------------------------------------------------ the shape
//
//  Two decisions carry this file, and neither is the obvious one.
//
//  1. The range is cut into many more chunks than there are workers, and
//     workers CLAIM chunks from a shared cursor rather than being handed a
//     fixed slice each. The obvious design - one slice per worker - is only
//     correct when every element costs the same, and that is exactly the
//     assumption that fails in practice: one slice lands on the expensive rows
//     and the other workers finish early and idle. Claiming means a worker that
//     drew cheap work comes back for more, so the imbalance costs one chunk
//     rather than one slice. This is the same reason the pool underneath steals.
//
//  2. THE CALLING THREAD RUNS CHUNKS TOO, and that is not (only) an
//     optimisation - it is what makes the call safe to nest. A parallel_for
//     invoked from inside a pool task occupies a worker while it waits; if it
//     waited passively and every worker did the same, the pool would have no
//     thread left to run the work they are all waiting on, and the program
//     would stop. Because the caller drains the same cursor, it can finish the
//     entire range alone. The submitted tasks then find the cursor exhausted
//     and retire without doing anything. Nested parallel_for cannot deadlock
//     here; it degrades to serial in the worst case, which is the right
//     failure.
//
//  A consequence worth stating: `body` is invoked concurrently on several
//  threads, and a chunk may run on the calling thread. It must be safe to call
//  from many threads at once, and it must not assume which thread it is on.
//  That is the same contract TBB's parallel_for carries.
//
//  What this is NOT: a scheduler. There is no global pool and no implicit
//  parallelism - the caller passes the pool it wants, because a library that
//  quietly spawns threads is a library you cannot put inside someone else's
//  thread budget.
//

#ifndef parallel_for_hpp
#define parallel_for_hpp

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <iterator>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

#include "../interfaces/task_pool.hpp"
#include "../utils/constexpr_for.hpp"

namespace snicholls
{
    namespace detail
    {
        // How many chunks to cut per worker. More chunks balance better and
        // cost one atomic increment each to claim; fewer chunks amortise that
        // increment over more work. Four is the usual answer and is what the
        // default grain below aims at - it is not tuned here because the right
        // number depends on how uneven the body is, which is why grain_size is
        // a parameter rather than a constant.
        inline constexpr std::size_t parallel_chunks_per_worker = 4;

        // Outstanding-chunk tracker. Same shape as the pool's own completion
        // gate, and for the same reason: the hot path (a chunk finishing) only
        // touches the atomic, and the mutex is taken solely to publish the
        // reached-zero wakeup. done() takes the lock BEFORE notifying rather
        // than after decrementing, which is what stops a waiter that has
        // evaluated its predicate but not yet slept from missing the signal
        // and waiting forever.
        class chunk_gate
        {
        public:
            void arm(std::size_t n) noexcept { outstanding_.store(n, std::memory_order_release); }

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

        private:
            std::atomic<std::size_t> outstanding_{0};
            std::mutex               m_;
            std::condition_variable  cv_;
        };

        // Everything the workers and the caller share for one parallel_for.
        // Heap-allocated behind a shared_ptr because a submitted task may still
        // be retiring after the call has returned: it will find the cursor
        // exhausted and touch nothing but the gate, but it must find those
        // alive to do so.
        template <typename Index, typename Body, std::size_t Unroll>
        struct parallel_range {
            Index       first;
            std::size_t grain;
            std::size_t count;              // elements
            std::size_t chunks;
            Body        body;

            std::atomic<std::size_t> cursor{0};
            std::atomic<bool>        failed{false};
            chunk_gate               gate;

            std::mutex         err_m;
            std::exception_ptr error;

            parallel_range(Index f, std::size_t g, std::size_t n, std::size_t c, Body b)
                : first(f), grain(g), count(n), chunks(c), body(std::move(b)) {}

            void capture(std::exception_ptr e)
            {
                std::lock_guard<std::mutex> g(err_m);
                if (!error) {                       // first failure wins; the rest are consequences
                    error = e;
                    failed.store(true, std::memory_order_release);
                }
            }

            // Claim chunks until there are none left. Called by the pool's
            // workers AND by the calling thread.
            //
            // Note what happens after a body throws: this keeps CLAIMING but
            // stops EXECUTING. Breaking out instead would leave the unclaimed
            // chunks undecremented and the waiter parked on a gate that can
            // never reach zero - the failure path hanging is a worse bug than
            // the failure.
            void drain()
            {
                for (;;) {
                    const std::size_t c = cursor.fetch_add(1, std::memory_order_relaxed);
                    if (c >= chunks)
                        return;
                    if (!failed.load(std::memory_order_acquire)) {
                        const std::size_t lo = c * grain;
                        const std::size_t hi = (lo + grain < count) ? lo + grain : count;
                        try {
                            // Unroll == 1 is the plain loop, which is what the
                            // compiler wants to see when it is going to unroll
                            // or vectorise this itself.
                            if constexpr (Unroll <= 1) {
                                for (std::size_t k = lo; k < hi; ++k)
                                    body(static_cast<Index>(first + static_cast<Index>(k)));
                            } else {
                                unrolled_for<Unroll>(lo, hi, [this](std::size_t k) {
                                    body(static_cast<Index>(first + static_cast<Index>(k)));
                                });
                            }
                        } catch (...) {
                            capture(std::current_exception());
                        }
                    }
                    gate.done();
                }
            }
        };
    } // namespace detail

    // Run `body(i)` for every i in [first, last), across `pool`, and return
    // when all of them have completed.
    //
    // grain_size is elements per chunk; 0 (the default) picks one that gives
    // roughly four chunks per worker. Raise it when the body is tiny and the
    // per-chunk atomic starts to show; lower it when the body's cost varies a
    // lot between elements, which is when balance matters more than overhead.
    //
    // If the body throws, the first exception is rethrown here once every other
    // chunk has settled, and chunks not yet started are skipped. Elements
    // already in flight still finish - there is no way to interrupt them, and
    // pretending otherwise would just mean returning while they wrote to memory
    // the caller thinks is finished with.
    // Unroll is a compile-time chunk-inner unroll factor, default 1 (none).
    // parallel_for<8>(pool, 0, n, body) emits the body eight times per inner
    // iteration; see utils/constexpr_for.hpp for why that is usually not the
    // win it sounds like, and `make bench-parallel` for the measurement.
    template <std::size_t Unroll = 1, typename Index, typename Body>
    void parallel_for(task_pool& pool, Index first, Index last, Body body,
                      std::size_t grain_size = 0)
    {
        static_assert(std::is_integral_v<Index>, "parallel_for needs an integral index");

        if (!(first < last))
            return;
        const std::size_t count = static_cast<std::size_t>(last - first);

        // One thread available, or a range too small to be worth splitting:
        // run it here. No submit, no shared state, no atomics - the serial path
        // should not pay for the parallel one.
        const std::size_t workers = pool.worker_count();
        if (workers <= 1 || count == 1) {
            if constexpr (Unroll <= 1) {
                for (Index i = first; i < last; ++i)
                    body(i);
            } else {
                unrolled_for<Unroll>(first, last, body);
            }
            return;
        }

        std::size_t grain = grain_size;
        if (grain == 0) {
            const std::size_t want = workers * detail::parallel_chunks_per_worker;
            grain = (count + want - 1) / want;          // ceil, so chunks <= want
            if (grain == 0)
                grain = 1;
        }
        const std::size_t chunks = (count + grain - 1) / grain;

        auto st = std::make_shared<detail::parallel_range<Index, Body, Unroll>>(
            first, grain, count, chunks, std::move(body));
        st->gate.arm(chunks);

        // One task per worker, not one per chunk: the chunks are claimed from
        // the cursor, so W tasks are enough to occupy W workers, and the
        // type-erased submit is paid W times instead of once per chunk.
        const std::size_t helpers = (workers < chunks) ? workers : chunks;
        for (std::size_t i = 0; i < helpers; ++i)
            pool.submit([st] { st->drain(); });

        st->drain();                // the caller is a worker too - see the header
        st->gate.wait();

        if (st->error)
            std::rethrow_exception(st->error);
    }

    // Run `body(*it)` for every element in [first, last).
    //
    // Random-access iterators only, and that is a deliberate refusal rather
    // than an omission: chunking a forward iterator means walking it to find
    // each chunk boundary, so a "parallel" loop over a std::list would spend
    // more time seeking than working and would look like a performance bug in
    // the caller's code rather than in this decision. Copy into a vector first
    // and the cost is at least visible.
    template <std::size_t Unroll = 1, typename Iter, typename Body>
    void parallel_for_each(task_pool& pool, Iter first, Iter last, Body body,
                           std::size_t grain_size = 0)
    {
        using category = typename std::iterator_traits<Iter>::iterator_category;
        static_assert(std::is_base_of_v<std::random_access_iterator_tag, category>,
                      "parallel_for_each needs random-access iterators - "
                      "chunking anything else costs more than it saves");

        using diff = typename std::iterator_traits<Iter>::difference_type;
        parallel_for<Unroll>(
            pool, diff{0}, last - first,
            [first, &body](diff i) { body(*(first + i)); },
            grain_size);
    }
} // namespace snicholls

#endif /* parallel_for_hpp */
