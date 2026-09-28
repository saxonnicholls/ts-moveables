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
#include "task_group.hpp"          // completion_gate, error_slot

#include <algorithm>
#include <functional>
#include <vector>

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

        // ------------------------------------------------------- the engine
        //
        // Everything below is built on this: cut [0,count) into chunks, let any
        // number of threads CLAIM them from one cursor, and come back when all
        // of them have completed. parallel_for, parallel_reduce, parallel_scan
        // and parallel_sort differ only in what they do with a chunk - so the
        // claiming, the completion gate, the exception handling and the
        // caller-participates rule live here once rather than four times.
        //
        // The gate and the error slot are shared with task_group (see
        // task_group.hpp) for the same reason.
        template <typename Fn>
        struct chunk_run {
            std::size_t grain;
            std::size_t count;
            std::size_t chunks;
            Fn          fn;                 // void(chunk_index, lo, hi)

            std::atomic<std::size_t> cursor{0};
            completion_gate          gate;
            error_slot               err;

            chunk_run(std::size_t g, std::size_t n, std::size_t c, Fn f)
                : grain(g), count(n), chunks(c), fn(std::move(f)) {}

            // Claim chunks until there are none left. Run by the pool's workers
            // AND by the calling thread.
            //
            // Note what happens after a chunk throws: this keeps CLAIMING but
            // stops EXECUTING. Breaking out instead would leave the unclaimed
            // chunks undecremented and the waiter parked on a gate that can
            // never reach zero - a hanging failure path is worse than the
            // failure.
            void drain()
            {
                for (;;) {
                    const std::size_t c = cursor.fetch_add(1, std::memory_order_relaxed);
                    if (c >= chunks)
                        return;
                    if (!err.failed()) {
                        const std::size_t lo = c * grain;
                        const std::size_t hi = (lo + grain < count) ? lo + grain : count;
                        try {
                            fn(c, lo, hi);
                        } catch (...) {
                            err.capture(std::current_exception());
                        }
                    }
                    gate.done();
                }
            }
        };

        // Elements per chunk: the caller's choice, or one that gives roughly
        // four chunks per worker.
        inline std::size_t pick_grain(std::size_t count, std::size_t workers,
                                      std::size_t requested)
        {
            if (requested != 0)
                return requested;
            const std::size_t want = workers * parallel_chunks_per_worker;
            const std::size_t g = (count + want - 1) / want;     // ceil => chunks <= want
            return g == 0 ? 1 : g;
        }

        // Run fn over every chunk of [0,count) and return once all of them have
        // completed, rethrowing the first exception any chunk threw.
        //
        // The state is heap-allocated behind a shared_ptr because a submitted
        // task may still be retiring after this returns: it will find the cursor
        // exhausted and touch nothing but the gate, but it must find the gate
        // alive to do so. That is also why fn may safely capture the caller's
        // frame by reference - a late task never calls it.
        template <typename Fn>
        void run_chunks(task_pool& pool, std::size_t count, std::size_t grain, Fn fn)
        {
            const std::size_t chunks = (count + grain - 1) / grain;
            auto st = std::make_shared<chunk_run<Fn>>(grain, count, chunks, std::move(fn));
            st->gate.add(chunks);

            // One task per worker, not one per chunk: chunks are claimed from
            // the cursor, so W tasks occupy W workers and the type-erased
            // submit is paid W times instead of once per chunk.
            const std::size_t workers = pool.worker_count();
            const std::size_t helpers = (workers < chunks) ? workers : chunks;
            for (std::size_t i = 0; i < helpers; ++i)
                pool.submit([st] { st->drain(); });

            st->drain();                    // the caller is a worker too
            st->gate.wait();
            st->err.rethrow_if_failed();
        }

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

        const std::size_t grain = detail::pick_grain(count, workers, grain_size);
        detail::run_chunks(pool, count, grain,
            [first, &body](std::size_t, std::size_t lo, std::size_t hi) {
                if constexpr (Unroll <= 1) {
                    for (std::size_t k = lo; k < hi; ++k)
                        body(static_cast<Index>(first + static_cast<Index>(k)));
                } else {
                    unrolled_for<Unroll>(lo, hi, [&](std::size_t k) {
                        body(static_cast<Index>(first + static_cast<Index>(k)));
                    });
                }
            });
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

    // ------------------------------------------------------------ reduce
    //
    // Map each index to a value and fold the results: this IS map-reduce, which
    // is why there is no separate parallel_map_reduce. `map(i)` is the map,
    // `reduce(a,b)` is the fold, `identity` is the seed.
    //
    //     const double total = parallel_reduce(pool, 0, n, 0.0,
    //                              [&](int i) { return v[i] * w[i]; },
    //                              std::plus<double>{});
    //
    // THE RESULT IS DETERMINISTIC, and that is a deliberate choice worth the
    // paragraph. Each chunk folds its own elements into a local accumulator,
    // and the per-chunk results are then folded IN CHUNK ORDER - never in
    // completion order. So the answer does not depend on how the scheduler
    // happened to interleave the work, and a floating-point sum is reproducible
    // run to run. Reducing in completion order is the obvious implementation and
    // is what TBB's parallel_reduce does by default; it is also how a
    // "nondeterministic" total sneaks into a test suite and then into a
    // support ticket about numbers that will not reconcile. The cost is one
    // value per chunk, and chunks are a small multiple of the worker count.
    //
    // `reduce` must be ASSOCIATIVE. It does NOT need to be commutative, because
    // the fold order is index order - so string concatenation and matrix
    // products are fine here, which they would not be in a completion-ordered
    // reduction.
    //
    // `identity` must be a real identity for `reduce`: every chunk seeds its
    // local accumulator with it, so a wrong seed is counted once per chunk
    // rather than once overall.
    template <std::size_t Unroll = 1, typename Index, typename T,
              typename Map, typename Reduce>
    T parallel_reduce(task_pool& pool, Index first, Index last, T identity,
                      Map map, Reduce reduce, std::size_t grain_size = 0)
    {
        static_assert(std::is_integral_v<Index>, "parallel_reduce needs an integral index");

        if (!(first < last))
            return identity;
        const std::size_t count = static_cast<std::size_t>(last - first);

        const std::size_t workers = pool.worker_count();
        if (workers <= 1 || count == 1) {
            T acc = identity;
            for (Index i = first; i < last; ++i)
                acc = reduce(std::move(acc), map(i));
            return acc;
        }

        const std::size_t grain  = detail::pick_grain(count, workers, grain_size);
        const std::size_t chunks = (count + grain - 1) / grain;

        // One slot per chunk, indexed by chunk id - this vector is what makes
        // the result independent of scheduling. Sized up front so no chunk ever
        // touches another's slot and no lock is needed.
        std::vector<T> partials(chunks, identity);

        detail::run_chunks(pool, count, grain,
            [&](std::size_t c, std::size_t lo, std::size_t hi) {
                T acc = identity;
                if constexpr (Unroll <= 1) {
                    for (std::size_t k = lo; k < hi; ++k)
                        acc = reduce(std::move(acc), map(static_cast<Index>(first + static_cast<Index>(k))));
                } else {
                    unrolled_for<Unroll>(lo, hi, [&](std::size_t k) {
                        acc = reduce(std::move(acc), map(static_cast<Index>(first + static_cast<Index>(k))));
                    });
                }
                partials[c] = std::move(acc);
            });

        T out = identity;                   // in chunk order, always
        for (auto& p : partials)
            out = reduce(std::move(out), std::move(p));
        return out;
    }

    // The iterator form: map sees the element, not the index.
    template <std::size_t Unroll = 1, typename Iter, typename T,
              typename Map, typename Reduce>
    T parallel_reduce_each(task_pool& pool, Iter first, Iter last, T identity,
                           Map map, Reduce reduce, std::size_t grain_size = 0)
    {
        using category = typename std::iterator_traits<Iter>::iterator_category;
        static_assert(std::is_base_of_v<std::random_access_iterator_tag, category>,
                      "parallel_reduce_each needs random-access iterators");
        using diff = typename std::iterator_traits<Iter>::difference_type;
        return parallel_reduce<Unroll>(
            pool, diff{0}, last - first, identity,
            [first, &map](diff i) { return map(*(first + i)); },
            reduce, grain_size);
    }


    // -------------------------------------------------------------- scan
    //
    // Prefix fold: out[i] is the combination of everything up to i. Inclusive
    // includes element i, exclusive does not; both return the grand total.
    //
    //     parallel_inclusive_scan(pool, v.begin(), v.end(), out.begin(),
    //                             0.0, std::plus<double>{});
    //
    // A scan looks inherently serial - every output depends on the one before -
    // and the trick is that it is only serial WITHIN a chunk. Two passes:
    //
    //   1. each chunk folds its own elements into a local total
    //   2. the chunk totals are prefixed serially (there are only a handful)
    //   3. each chunk re-walks its range, seeded with its own offset
    //
    // Note the cost that buys: the data is traversed TWICE, so a parallel scan
    // moves about twice the memory a serial one does and only wins once there
    // are enough cores to pay that back. On a memory-bound body that can mean
    // never - `make bench-parallel` prints the crossover rather than assuming
    // it. Deterministic for the same reason parallel_reduce is: the offsets are
    // accumulated in chunk order, so `combine` must be associative but need not
    // be commutative.
    namespace detail
    {
        template <bool Inclusive, typename Iter, typename OutIter, typename T, typename Combine>
        T scan_impl(task_pool& pool, Iter first, Iter last, OutIter out,
                    T identity, Combine combine, std::size_t grain_size)
        {
            using category = typename std::iterator_traits<Iter>::iterator_category;
            static_assert(std::is_base_of_v<std::random_access_iterator_tag, category>,
                          "parallel scan needs random-access iterators");

            if (!(first < last))
                return identity;
            const std::size_t count = static_cast<std::size_t>(last - first);

            const std::size_t workers = pool.worker_count();
            if (workers <= 1 || count == 1) {
                T run = identity;
                for (std::size_t k = 0; k < count; ++k) {
                    if constexpr (Inclusive) {
                        run = combine(std::move(run), *(first + static_cast<std::ptrdiff_t>(k)));
                        *(out + static_cast<std::ptrdiff_t>(k)) = run;
                    } else {
                        *(out + static_cast<std::ptrdiff_t>(k)) = run;
                        run = combine(std::move(run), *(first + static_cast<std::ptrdiff_t>(k)));
                    }
                }
                return run;
            }

            const std::size_t grain  = pick_grain(count, workers, grain_size);
            const std::size_t chunks = (count + grain - 1) / grain;

            // pass 1: per-chunk totals
            std::vector<T> totals(chunks, identity);
            run_chunks(pool, count, grain,
                [&](std::size_t c, std::size_t lo, std::size_t hi) {
                    T acc = identity;
                    for (std::size_t k = lo; k < hi; ++k)
                        acc = combine(std::move(acc), *(first + static_cast<std::ptrdiff_t>(k)));
                    totals[c] = std::move(acc);
                });

            // pass 2 (serial, chunks is small): the offset each chunk starts at
            std::vector<T> offsets(chunks, identity);
            T running = identity;
            for (std::size_t c = 0; c < chunks; ++c) {
                offsets[c] = running;
                running = combine(std::move(running), totals[c]);
            }

            // pass 3: re-walk each chunk from its own offset
            run_chunks(pool, count, grain,
                [&](std::size_t c, std::size_t lo, std::size_t hi) {
                    T run = offsets[c];
                    for (std::size_t k = lo; k < hi; ++k) {
                        if constexpr (Inclusive) {
                            run = combine(std::move(run), *(first + static_cast<std::ptrdiff_t>(k)));
                            *(out + static_cast<std::ptrdiff_t>(k)) = run;
                        } else {
                            *(out + static_cast<std::ptrdiff_t>(k)) = run;
                            run = combine(std::move(run), *(first + static_cast<std::ptrdiff_t>(k)));
                        }
                    }
                });

            return running;
        }
    } // namespace detail

    template <typename Iter, typename OutIter, typename T, typename Combine>
    T parallel_inclusive_scan(task_pool& pool, Iter first, Iter last, OutIter out,
                              T identity, Combine combine, std::size_t grain_size = 0)
    {
        return detail::scan_impl<true>(pool, first, last, out, identity, combine, grain_size);
    }

    template <typename Iter, typename OutIter, typename T, typename Combine>
    T parallel_exclusive_scan(task_pool& pool, Iter first, Iter last, OutIter out,
                              T identity, Combine combine, std::size_t grain_size = 0)
    {
        return detail::scan_impl<false>(pool, first, last, out, identity, combine, grain_size);
    }

    // -------------------------------------------------------------- sort
    //
    //     parallel_sort(pool, v.begin(), v.end());
    //     parallel_sort(pool, v.begin(), v.end(), std::greater<>{});
    //
    // Sort k runs in parallel, then merge them pairwise in log2(k) rounds, each
    // round's merges running in parallel. The per-run sort and the merge are
    // the standard library's - std::sort and std::inplace_merge - because those
    // are heavily tuned and there is nothing to gain by writing a worse
    // quicksort here. What this adds is the scheduling.
    //
    // Two honest caveats. std::inplace_merge allocates a temporary when it can,
    // so this is not an allocation-free sort; without the temporary it still
    // works but degrades. And the merge rounds halve the available parallelism
    // each time - the last round is a single merge on one thread - so the
    // speedup ceiling is well under the core count. Measured, not asserted:
    // see `make bench-parallel`.
    //
    // parallel_sort is UNSTABLE (std::sort per run); parallel_stable_sort uses
    // std::stable_sort and is stable throughout, since the merge is stable.
    namespace detail
    {
        // Runs shorter than this are not worth a thread of their own.
        inline constexpr std::size_t parallel_sort_min_run = 1u << 12;

        template <typename Iter, typename Compare, typename RunSort>
        void sort_impl(task_pool& pool, Iter first, Iter last, Compare comp, RunSort run_sort)
        {
            using category = typename std::iterator_traits<Iter>::iterator_category;
            static_assert(std::is_base_of_v<std::random_access_iterator_tag, category>,
                          "parallel sort needs random-access iterators");
            using diff = typename std::iterator_traits<Iter>::difference_type;

            const diff n = last - first;
            if (n < 2)
                return;

            const std::size_t workers = pool.worker_count();
            if (workers <= 1 || static_cast<std::size_t>(n) <= parallel_sort_min_run) {
                run_sort(first, last, comp);
                return;
            }

            // A power-of-two run count keeps the merge tree exact, and no run
            // smaller than the floor above.
            std::size_t runs = 1;
            while (runs * 2 <= workers &&
                   static_cast<std::size_t>(n) / (runs * 2) >= parallel_sort_min_run)
                runs *= 2;

            const diff run_len = (n + static_cast<diff>(runs) - 1) / static_cast<diff>(runs);
            auto bound = [&](std::size_t k) {
                const diff v = static_cast<diff>(k) * run_len;
                return v < n ? v : n;
            };

            parallel_for(pool, std::size_t{0}, runs, [&](std::size_t r) {
                run_sort(first + bound(r), first + bound(r + 1), comp);
            }, 1);

            for (std::size_t width = 1; width < runs; width *= 2) {
                const std::size_t pairs = (runs + 2 * width - 1) / (2 * width);
                parallel_for(pool, std::size_t{0}, pairs, [&](std::size_t p) {
                    const diff lo  = bound(p * 2 * width);
                    const diff mid = bound(p * 2 * width + width);
                    const diff hi  = bound((p + 1) * 2 * width);
                    if (mid < hi)
                        std::inplace_merge(first + lo, first + mid, first + hi, comp);
                }, 1);
            }
        }
    } // namespace detail

    template <typename Iter, typename Compare = std::less<>>
    void parallel_sort(task_pool& pool, Iter first, Iter last, Compare comp = Compare{})
    {
        detail::sort_impl(pool, first, last, comp,
                          [](auto b, auto e, auto c) { std::sort(b, e, c); });
    }

    template <typename Iter, typename Compare = std::less<>>
    void parallel_stable_sort(task_pool& pool, Iter first, Iter last, Compare comp = Compare{})
    {
        detail::sort_impl(pool, first, last, comp,
                          [](auto b, auto e, auto c) { std::stable_sort(b, e, c); });
    }

} // namespace snicholls

#endif /* parallel_for_hpp */
