//
//  work_stealing_deque.hpp
//  TSMoveables
//
//  Created by Saxon Nicholls on 22/7/2026.
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables
//
//  A bounded Chase-Lev work-stealing deque with the memory orderings from
//  Le, Pop, Cohen & Zappa Nardelli, "Correct and Efficient Work-Stealing for
//  Weak Memory Models" (PPoPP 2013) - the version verified with CDSChecker.
//
//  One owner thread pushes and pops the bottom (LIFO, cache-friendly); any
//  number of thieves steal from the top (FIFO). Elements are pointers, so the
//  single-element race (owner pop and a thief both read the same slot, then a
//  CAS on `top` decides the winner) only ever duplicates a *pointer* read -
//  never touches the pointee - and the loser discards its copy. Ownership of
//  the pointee is unambiguous: exactly one of pop/steal returns it.
//

#ifndef work_stealing_deque_hpp
#define work_stealing_deque_hpp

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace snicholls
{
    // Bounded so there is no array growth, and therefore no deferred-reclaim
    // hazard for retired buffers - push simply fails when full and the caller
    // routes the item elsewhere (e.g. an injector queue).
    template <typename T>
    class work_stealing_deque {

        static constexpr std::size_t cache_line_size =
#if defined(__aarch64__) || defined(_M_ARM64)
            128;
#else
            64;
#endif

        static std::size_t round_up_pow2(std::size_t n) noexcept {
            std::size_t p = 1;
            while (p < n)
                p <<= 1;
            return p;
        }

        std::size_t mask_;
        std::unique_ptr<std::atomic<T*>[]> slots_;
        alignas(cache_line_size) std::atomic<std::int64_t> top_{0};
        alignas(cache_line_size) std::atomic<std::int64_t> bottom_{0};

    public:
        explicit work_stealing_deque(std::size_t capacity)
            : mask_(round_up_pow2(capacity < 2 ? 2 : capacity) - 1),
              slots_(std::make_unique<std::atomic<T*>[]>(mask_ + 1)) {}

        work_stealing_deque(const work_stealing_deque&) = delete;
        work_stealing_deque& operator=(const work_stealing_deque&) = delete;

        std::size_t capacity() const noexcept { return mask_ + 1; }

        // Owner only. Returns false if the deque is full.
        bool push(T* x) {
            const std::int64_t b = bottom_.load(std::memory_order_relaxed);
            const std::int64_t t = top_.load(std::memory_order_acquire);
            if (b - t >= static_cast<std::int64_t>(mask_ + 1))
                return false;                   // full
            // Publish the element with release on the slot AND on bottom,
            // rather than a standalone release fence. ThreadSanitizer does not
            // model std::atomic_thread_fence (GCC even warns -Wtsan), so a
            // fence-based handoff makes it false-positive on the element a
            // producer wrote and a thief later runs. Release/acquire on the
            // actual atomics carries the identical happens-before and TSan
            // understands it - and a release fence before a relaxed store is,
            // by the memory model, exactly a release store.
            slots_[static_cast<std::size_t>(b) & mask_].store(x, std::memory_order_release);
            bottom_.store(b + 1, std::memory_order_release);
            return true;
        }

        // Owner only. Returns nullptr if the deque is empty. LIFO.
        T* pop() {
            const std::int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
            bottom_.store(b, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            std::int64_t t = top_.load(std::memory_order_relaxed);
            T* x = nullptr;
            if (t <= b) {
                x = slots_[static_cast<std::size_t>(b) & mask_].load(std::memory_order_relaxed);
                if (t == b) {
                    // Last element - a thief may be taking it too; the CAS decides
                    if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                                      std::memory_order_relaxed))
                        x = nullptr;            // lost the race to a thief
                    bottom_.store(b + 1, std::memory_order_relaxed);
                }
                // else: more than one element - x is ours, no race
            } else {
                // Was already empty
                bottom_.store(b + 1, std::memory_order_relaxed);
            }
            return x;
        }

        // Any thief. Returns nullptr if empty. Sets lost=true (and returns
        // nullptr) if another thread won the same slot - the caller should try
        // a different victim rather than conclude the deque is empty. FIFO.
        T* steal(bool& lost) {
            lost = false;
            std::int64_t t = top_.load(std::memory_order_acquire);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            const std::int64_t b = bottom_.load(std::memory_order_acquire);
            if (t < b) {
                // Acquire pairs with push()'s release on the slot, so the
                // element the producer built happens-before we return and run
                // it (the synchronisation TSan can actually see).
                T* x = slots_[static_cast<std::size_t>(t) & mask_].load(std::memory_order_acquire);
                if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                                  std::memory_order_relaxed)) {
                    lost = true;
                    return nullptr;
                }
                return x;
            }
            return nullptr;                     // empty
        }

        // Approximate; owner-consistent, thief-racy. For diagnostics only.
        std::size_t size() const noexcept {
            const std::int64_t b = bottom_.load(std::memory_order_relaxed);
            const std::int64_t t = top_.load(std::memory_order_relaxed);
            return b > t ? static_cast<std::size_t>(b - t) : 0;
        }
    };

} // namespace snicholls

#endif /* work_stealing_deque_hpp */
