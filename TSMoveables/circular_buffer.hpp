//
//  circular_buffer.hpp
//  TSMoveables
//
//  Created by Saxon Nicholls on 21/7/2026.
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables
//

#ifndef circular_buffer_hpp
#define circular_buffer_hpp

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace snicholls
{
    // Moveable SPSC circular buffer
    //
    // A fixed-capacity single-producer / single-consumer ring whose entire
    // concurrent state is two indices you can point at: tail (producer) and
    // head (consumer), each a std::atomic<std::size_t> on its own cache line.
    // Indices increase monotonically and wrap by mask - capacity is a power of
    // two, full/empty are distinguished by the difference, no slot is wasted.
    // push and pop are wait-free: one relaxed load, one acquire load at most
    // (usually amortised away by the cached index), one release store.
    //
    //   circular_buffer<T>       - capacity chosen at construction (rounded up
    //                              to a power of two), one allocation, ever
    //   circular_buffer<T, N>    - compile-time capacity, zero allocation
    //
    // Thread contract: at most one thread pushes and one thread pops at any
    // moment (they may be the same thread). size()/empty()/full()/capacity()
    // are safe from any thread and are snapshots.
    //
    // Move and copy follow the library's quiescent contract, with an honest
    // caveat: a lock-free ring has no lock to probe, so the check is
    // best-effort - each side sets an "active" flag (two relaxed stores to a
    // cache line that side already owns, effectively free) and move/copy
    // throws std::runtime_error if either flag is up. It makes concurrent
    // misuse loud; it does not make move-during-push a sensible program.
    // Moves transfer the queued elements in order and leave the source empty
    // and fully usable, with its capacity and storage intact.
    template <typename T, std::size_t N = 0>
    struct circular_buffer {

        static_assert(N == 0 || (N & (N - 1)) == 0,
                      "circular_buffer: compile-time capacity must be a power of two");

        using value_type = T;

        static constexpr bool has_static_capacity = (N != 0);

        // Apple Silicon and other modern AArch64 cores pair-prefetch 128 bytes;
        // everything else common is 64. (std::hardware_destructive_interference_size
        // exists but triggers -Winterference-size churn on GCC, so we spell it out.)
        static constexpr std::size_t cache_line_size =
#if defined(__aarch64__) || defined(_M_ARM64)
            128;
#else
            64;
#endif

    private:
        struct slot {
            alignas(alignof(T)) unsigned char bytes[sizeof(T)];
        };
        using storage_type = std::conditional_t<has_static_capacity,
                                                std::array<slot, (has_static_capacity ? N : 1)>,
                                                std::unique_ptr<slot[]>>;

        // Consumer side: head_ is written only by the consumer.
        // tail_cache_ is the consumer's private (possibly stale) view of tail_,
        // so the common pop touches no cache line the producer writes.
        alignas(cache_line_size) std::atomic<std::size_t> head_{0};
        std::size_t tail_cache_{0};
        std::atomic<bool> consumer_active_{false};

        // Producer side: tail_ is written only by the producer.
        alignas(cache_line_size) std::atomic<std::size_t> tail_{0};
        std::size_t head_cache_{0};
        std::atomic<bool> producer_active_{false};

        // Cold: read-only after construction (except assignment)
        alignas(cache_line_size) std::size_t mask_ = has_static_capacity ? N - 1 : 0;
        storage_type storage_{};

        // Two relaxed stores to a line the side already owns - effectively free
        struct side_guard {
            std::atomic<bool>& flag;
            explicit side_guard(std::atomic<bool>& f) noexcept : flag(f) {
                flag.store(true, std::memory_order_relaxed);
            }
            ~side_guard() { flag.store(false, std::memory_order_release); }
        };

        static void ensure_quiescent(const circular_buffer& other) {
            if (other.producer_active_.load(std::memory_order_acquire) ||
                other.consumer_active_.load(std::memory_order_acquire))
                throw std::runtime_error("circular_buffer: moving while operations are in flight");
        }

        static constexpr std::size_t round_up_pow2(std::size_t n) noexcept {
            std::size_t p = 1;
            while (p < n)
                p <<= 1;
            return p;
        }

        slot* data() noexcept {
            if constexpr (has_static_capacity)
                return storage_.data();
            else
                return storage_.get();
        }
        const slot* data() const noexcept {
            if constexpr (has_static_capacity)
                return storage_.data();
            else
                return storage_.get();
        }

        // idx is a monotonic index; masking happens here
        void* slot_bytes(std::size_t idx) noexcept { return data()[idx & mask_].bytes; }
        T* slot_ptr(std::size_t idx) noexcept {
            return std::launder(reinterpret_cast<T*>(data()[idx & mask_].bytes));
        }
        const T* slot_ptr(std::size_t idx) const noexcept {
            return std::launder(reinterpret_cast<const T*>(data()[idx & mask_].bytes));
        }

        void allocate_storage() {
            if constexpr (!has_static_capacity)
                storage_.reset(new slot[mask_ + 1]);
        }

        // Requires: *this empty, capacity == other's. Leaves other empty.
        void steal_elements(circular_buffer& other) noexcept(std::is_nothrow_move_constructible_v<T>) {
            const std::size_t h = other.head_.load(std::memory_order_relaxed);
            const std::size_t t = other.tail_.load(std::memory_order_relaxed);
            for (std::size_t i = h; i != t; ++i) {
                T* src = other.slot_ptr(i);
                ::new (slot_bytes(i - h)) T(std::move(*src));
                src->~T();
            }
            head_.store(0, std::memory_order_relaxed);
            tail_.store(t - h, std::memory_order_relaxed);
            head_cache_ = 0;
            tail_cache_ = 0;
            other.head_.store(0, std::memory_order_relaxed);
            other.tail_.store(0, std::memory_order_relaxed);
            other.head_cache_ = 0;
            other.tail_cache_ = 0;
        }

        // Requires: *this empty, capacity == other's. Leaves other untouched.
        void copy_elements(const circular_buffer& other) {
            const std::size_t h = other.head_.load(std::memory_order_relaxed);
            const std::size_t t = other.tail_.load(std::memory_order_relaxed);
            std::size_t constructed = 0;
            try {
                for (std::size_t i = h; i != t; ++i, ++constructed)
                    ::new (slot_bytes(i - h)) T(*other.slot_ptr(i));
            } catch (...) {
                for (std::size_t i = 0; i < constructed; ++i)
                    slot_ptr(i)->~T();
                throw;
            }
            head_.store(0, std::memory_order_relaxed);
            tail_.store(t - h, std::memory_order_relaxed);
            head_cache_ = 0;
            tail_cache_ = 0;
        }

        void destroy_elements() noexcept {
            const std::size_t h = head_.load(std::memory_order_relaxed);
            const std::size_t t = tail_.load(std::memory_order_relaxed);
            for (std::size_t i = h; i != t; ++i)
                slot_ptr(i)->~T();
        }

    public:
        // Compile-time capacity
        template <bool B = has_static_capacity, std::enable_if_t<B, int> = 0>
        circular_buffer() noexcept {}

        // Runtime capacity - rounded up to the next power of two
        template <bool B = has_static_capacity, std::enable_if_t<!B, int> = 0>
        explicit circular_buffer(std::size_t capacity) {
            if (capacity == 0)
                throw std::invalid_argument("circular_buffer: capacity must be positive");
            mask_ = round_up_pow2(capacity) - 1;
            allocate_storage();
        }

        circular_buffer(circular_buffer&& other) : mask_(other.mask_) {
            ensure_quiescent(other);
            allocate_storage();
            steal_elements(other);
        }

        circular_buffer(const circular_buffer& other) : mask_(other.mask_) {
            ensure_quiescent(other);
            allocate_storage();
            copy_elements(other);
        }

        circular_buffer& operator=(circular_buffer&& other) {
            if (this != &other) {
                ensure_quiescent(*this);
                ensure_quiescent(other);
                destroy_elements();
                if constexpr (!has_static_capacity) {
                    if (mask_ != other.mask_) {
                        mask_ = other.mask_;
                        allocate_storage();
                    }
                }
                steal_elements(other);
            }
            return *this;
        }

        circular_buffer& operator=(const circular_buffer& other) {
            if (this != &other) {
                ensure_quiescent(*this);
                ensure_quiescent(other);
                destroy_elements();
                head_.store(0, std::memory_order_relaxed);      // empty while we copy
                tail_.store(0, std::memory_order_relaxed);
                if constexpr (!has_static_capacity) {
                    if (mask_ != other.mask_) {
                        mask_ = other.mask_;
                        allocate_storage();
                    }
                }
                copy_elements(other);
            }
            return *this;
        }

        ~circular_buffer() { destroy_elements(); }

        // ------------------------------------------------------ producer side

        // Conditionally noexcept throughout: the ring's own machinery (atomics,
        // index arithmetic, placement new) never throws - only T's operations can
        template <typename... Args>
        bool try_emplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args&&...>) {
            side_guard g(producer_active_);
            // Relaxed: only the producer writes tail_, so this is our own value
            const std::size_t t = tail_.load(std::memory_order_relaxed);
            if (t - head_cache_ > mask_) {              // appears full - refresh the cache
                // Acquire: pairs with the consumer's release store of head_, so the
                // consumer's destruction of the old occupant of our slot
                // happens-before we construct into it
                head_cache_ = head_.load(std::memory_order_acquire);
                if (t - head_cache_ > mask_)
                    return false;                       // truly full
            }
            ::new (slot_bytes(t)) T(std::forward<Args>(args)...);
            // Release: pairs with the consumer's acquire load of tail_, publishing
            // the element we just constructed
            tail_.store(t + 1, std::memory_order_release);
            return true;
        }

        bool try_push(const T& v) noexcept(std::is_nothrow_copy_constructible_v<T>) {
            return try_emplace(v);
        }
        bool try_push(T&& v) noexcept(std::is_nothrow_move_constructible_v<T>) {
            return try_emplace(std::move(v));
        }

        // Copy up to n elements from first; returns how many were pushed.
        // Amortises the atomic traffic: one acquire at most, one release total.
        // Pass std::make_move_iterator(first) to move instead of copy.
        template <typename InputIt>
        std::size_t push_n(InputIt first, std::size_t n) {
            side_guard g(producer_active_);
            const std::size_t t = tail_.load(std::memory_order_relaxed);
            std::size_t free_slots = capacity() - (t - head_cache_);
            if (free_slots < n) {
                head_cache_ = head_.load(std::memory_order_acquire);
                free_slots = capacity() - (t - head_cache_);
            }
            const std::size_t count = n < free_slots ? n : free_slots;
            for (std::size_t i = 0; i < count; ++i, ++first)
                ::new (slot_bytes(t + i)) T(*first);
            if (count != 0)
                tail_.store(t + count, std::memory_order_release);
            return count;
        }

        // ------------------------------------------------------ consumer side

        bool try_pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>) {
            side_guard g(consumer_active_);
            // Relaxed: only the consumer writes head_
            const std::size_t h = head_.load(std::memory_order_relaxed);
            if (h == tail_cache_) {                     // appears empty - refresh the cache
                // Acquire: pairs with the producer's release store of tail_, making
                // the constructed element visible before we read it
                tail_cache_ = tail_.load(std::memory_order_acquire);
                if (h == tail_cache_)
                    return false;                       // truly empty
            }
            T* p = slot_ptr(h);
            out = std::move(*p);
            p->~T();
            // Release: pairs with the producer's acquire load of head_, so our
            // destruction of this slot happens-before the producer reuses it
            head_.store(h + 1, std::memory_order_release);
            return true;
        }

        std::optional<T> try_pop() noexcept(std::is_nothrow_move_constructible_v<T>) {
            side_guard g(consumer_active_);
            const std::size_t h = head_.load(std::memory_order_relaxed);
            if (h == tail_cache_) {
                tail_cache_ = tail_.load(std::memory_order_acquire);
                if (h == tail_cache_)
                    return std::nullopt;
            }
            T* p = slot_ptr(h);
            std::optional<T> out(std::move(*p));
            p->~T();
            head_.store(h + 1, std::memory_order_release);
            return out;
        }

        // Move up to max_n elements into out; returns how many were popped
        template <typename OutputIt>
        std::size_t pop_n(OutputIt out, std::size_t max_n) {
            side_guard g(consumer_active_);
            const std::size_t h = head_.load(std::memory_order_relaxed);
            std::size_t available = tail_cache_ - h;
            if (available < max_n) {
                tail_cache_ = tail_.load(std::memory_order_acquire);
                available = tail_cache_ - h;
            }
            const std::size_t count = max_n < available ? max_n : available;
            for (std::size_t i = 0; i < count; ++i, ++out) {
                T* p = slot_ptr(h + i);
                *out = std::move(*p);
                p->~T();
            }
            if (count != 0)
                head_.store(h + count, std::memory_order_release);
            return count;
        }

        // Discard everything queued (a consumer-side operation)
        void clear() noexcept {
            side_guard g(consumer_active_);
            const std::size_t h = head_.load(std::memory_order_relaxed);
            const std::size_t t = tail_.load(std::memory_order_acquire);
            for (std::size_t i = h; i != t; ++i)
                slot_ptr(i)->~T();
            head_.store(t, std::memory_order_release);
        }

        // ------------------------------------------------- any-thread queries
        // Snapshots: true at the moment they were computed

        std::size_t size() const noexcept {
            // head first: elements popped between the two loads make us
            // overcount, never underflow
            const std::size_t h = head_.load(std::memory_order_acquire);
            const std::size_t t = tail_.load(std::memory_order_acquire);
            return t - h;
        }

        bool empty() const noexcept { return size() == 0; }
        bool full() const noexcept { return size() >= capacity(); }
        std::size_t capacity() const noexcept { return mask_ + 1; }
    };

} // namespace snicholls

#endif /* circular_buffer_hpp */
