//
//  mpmc_queue.hpp
//  TSMoveables
//
//  Created by Saxon Nicholls on 22/7/2026.
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables
//
//  After: Dmitry Vyukov, "Bounded MPMC queue" (1024cores.net) - a bounded,
//  lock-free multi-producer / multi-consumer ring. Each cell carries a
//  sequence number; producers and consumers CAS a shared ticket and then read
//  the cell's sequence to know whether the cell is theirs. No cell is ever
//  contended by two producers or two consumers at once, so the element itself
//  needs no extra synchronisation.
//

#ifndef mpmc_queue_hpp
#define mpmc_queue_hpp

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <utility>

namespace snicholls
{
    // Moveable bounded MPMC queue.
    //
    // Lock-free from any number of producers and consumers. Capacity is a
    // power of two (rounded up). push/try_pop are wait-free per attempt (a
    // bounded CAS retry only when another thread races the same ticket).
    //
    // Move and copy follow the library's quiescent contract in spirit -
    // contents and positions transfer, the source is left empty - but unlike
    // the SPSC ring there is no cheap per-side flag to probe here (a shared
    // in-flight counter would contend the very hot path the per-cell design
    // exists to keep clean). So the move is unchecked and is the caller's
    // contract: move only a quiescent queue. This is stated, not hidden.
    //
    // T must be nothrow-move-constructible for the element to transit a cell
    // safely (as the algorithm assumes); most task/message types are.
    template <typename T>
    class mpmc_queue {

        static constexpr std::size_t cache_line_size =
#if defined(__aarch64__) || defined(_M_ARM64)
            128;
#else
            64;
#endif

        struct cell {
            std::atomic<std::size_t> seq;
            alignas(alignof(T)) unsigned char storage[sizeof(T)];

            T* ptr() noexcept { return std::launder(reinterpret_cast<T*>(storage)); }
        };

        // Cold (constant after construction)
        std::unique_ptr<cell[]> buffer_;
        std::size_t mask_ = 0;

        // Producer and consumer tickets on their own cache lines
        alignas(cache_line_size) std::atomic<std::size_t> enqueue_pos_{0};
        alignas(cache_line_size) std::atomic<std::size_t> dequeue_pos_{0};

        static std::size_t round_up_pow2(std::size_t n) noexcept {
            std::size_t p = 1;
            while (p < n)
                p <<= 1;
            return p;
        }

        void init(std::size_t capacity) {
            const std::size_t c = round_up_pow2(capacity < 2 ? 2 : capacity);
            mask_ = c - 1;
            buffer_ = std::make_unique<cell[]>(c);
            for (std::size_t i = 0; i < c; ++i)
                buffer_[i].seq.store(i, std::memory_order_relaxed);
        }

    public:
        using value_type = T;

        explicit mpmc_queue(std::size_t capacity) { init(capacity); }

        mpmc_queue(const mpmc_queue&) = delete;
        mpmc_queue& operator=(const mpmc_queue&) = delete;

        mpmc_queue(mpmc_queue&& other) noexcept
            : buffer_(std::move(other.buffer_)), mask_(other.mask_) {
            enqueue_pos_.store(other.enqueue_pos_.load(std::memory_order_relaxed),
                               std::memory_order_relaxed);
            dequeue_pos_.store(other.dequeue_pos_.load(std::memory_order_relaxed),
                               std::memory_order_relaxed);
            other.mask_ = 0;                    // moved-from is empty; buffer_ is null
        }

        mpmc_queue& operator=(mpmc_queue&& other) noexcept {
            if (this != &other) {
                drain();
                buffer_ = std::move(other.buffer_);
                mask_ = other.mask_;
                enqueue_pos_.store(other.enqueue_pos_.load(std::memory_order_relaxed),
                                   std::memory_order_relaxed);
                dequeue_pos_.store(other.dequeue_pos_.load(std::memory_order_relaxed),
                                   std::memory_order_relaxed);
                other.mask_ = 0;
            }
            return *this;
        }

        ~mpmc_queue() { drain(); }

        // Construct an element in place. Returns false if the queue is full.
        template <typename... Args>
        bool emplace(Args&&... args) {
            cell* c;
            std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
            for (;;) {
                c = &buffer_[pos & mask_];
                const std::size_t seq = c->seq.load(std::memory_order_acquire);
                const std::intptr_t dif =
                    static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
                if (dif == 0) {
                    // Cell is free and ours to claim
                    if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                        break;
                } else if (dif < 0) {
                    return false;               // producer has lapped the consumer: full
                } else {
                    pos = enqueue_pos_.load(std::memory_order_relaxed);
                }
            }
            ::new (c->storage) T(std::forward<Args>(args)...);
            // Release: publishes the constructed element to the consumer that
            // will read this cell's sequence with acquire
            c->seq.store(pos + 1, std::memory_order_release);
            return true;
        }

        bool push(const T& v) { return emplace(v); }
        bool push(T&& v) { return emplace(std::move(v)); }

        // Move the front element into out. Returns false if the queue is empty.
        bool try_pop(T& out) {
            cell* c;
            std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
            for (;;) {
                c = &buffer_[pos & mask_];
                const std::size_t seq = c->seq.load(std::memory_order_acquire);
                const std::intptr_t dif =
                    static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);
                if (dif == 0) {
                    if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                        break;
                } else if (dif < 0) {
                    return false;               // consumer has caught the producer: empty
                } else {
                    pos = dequeue_pos_.load(std::memory_order_relaxed);
                }
            }
            T* p = c->ptr();
            out = std::move(*p);
            p->~T();
            // Release: frees the cell for a producer one lap ahead
            c->seq.store(pos + mask_ + 1, std::memory_order_release);
            return true;
        }

        std::optional<T> try_pop() {
            cell* c;
            std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
            for (;;) {
                c = &buffer_[pos & mask_];
                const std::size_t seq = c->seq.load(std::memory_order_acquire);
                const std::intptr_t dif =
                    static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);
                if (dif == 0) {
                    if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                        break;
                } else if (dif < 0) {
                    return std::nullopt;
                } else {
                    pos = dequeue_pos_.load(std::memory_order_relaxed);
                }
            }
            T* p = c->ptr();
            std::optional<T> out(std::move(*p));
            p->~T();
            c->seq.store(pos + mask_ + 1, std::memory_order_release);
            return out;
        }

        // Approximate snapshots - the true value may shift under concurrency
        std::size_t capacity() const noexcept { return mask_ + 1; }

        std::size_t size() const noexcept {
            const std::size_t e = enqueue_pos_.load(std::memory_order_acquire);
            const std::size_t d = dequeue_pos_.load(std::memory_order_acquire);
            return e > d ? e - d : 0;
        }

        bool empty() const noexcept { return size() == 0; }

    private:
        void drain() noexcept {
            if (!buffer_)
                return;
            while (try_pop().has_value()) {      // destroys each remaining element
            }
        }
    };

} // namespace snicholls

#endif /* mpmc_queue_hpp */
