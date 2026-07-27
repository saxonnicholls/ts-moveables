//
//  moveable_atomic.hpp
//  TSMoveables
//
//  Created by Saxon Nicholls on 5/4/2024.
//
//  Copyright 2024 Saxon Herschel Nicholls
//
//  Thread Safe Moveables
//

#ifndef moveable_atomic_hpp
#define moveable_atomic_hpp

#include <atomic>
#include <cstdint>
#include <cstddef>

namespace snicholls
{
    // Moveable atomic
    // We use composition here rather than inheritance
    //
    // "Move" and "copy" both mean: atomically load the source value and store it
    // into a freshly constructed atomic. The source remains valid and unchanged.
    // This preserves the integrity of the value; it does not (and cannot) transfer
    // any in-flight atomic operations - callers must ensure the source is not being
    // concurrently mutated while it is being copied/moved, as with any object.
    template <typename T>
    struct moveable_atomic {

        using atomic_type = std::atomic<T>;
        using value_type = T;

        static constexpr bool is_always_lock_free = atomic_type::is_always_lock_free;

        atomic_type a;                          // a for atomic

        moveable_atomic() noexcept : a() {}
        // constexpr, like std::atomic's own value constructor: statics of this
        // type are constant-initialized, never dynamically initialized
        constexpr moveable_atomic(T t) noexcept : a(t) {}
        explicit moveable_atomic(const std::atomic<T>& other) noexcept : a(other.load()) {}

        moveable_atomic(const moveable_atomic& cpy) noexcept : a(cpy.a.load()) {}
        moveable_atomic(moveable_atomic&& mve) noexcept : a(mve.a.load()) {}

        moveable_atomic& operator=(const moveable_atomic& other) noexcept {
            a.store(other.a.load());
            return *this;
        }

        moveable_atomic& operator=(moveable_atomic&& other) noexcept {
            a.store(other.a.load());
            return *this;
        }

        moveable_atomic& operator=(const T& other) noexcept {
            a.store(other);
            return *this;
        }

        T get() const noexcept { return a.load(); }

        T load(std::memory_order order = std::memory_order_seq_cst) const noexcept { return a.load(order); }

        void store(const T& value, std::memory_order order = std::memory_order_seq_cst) noexcept {
            a.store(value, order);
        }

        T exchange(const T& value, std::memory_order order = std::memory_order_seq_cst) noexcept {
            return a.exchange(value, order);
        }

        bool compare_exchange_weak(T& expected, const T& value, std::memory_order success = std::memory_order_seq_cst,
                                    std::memory_order failure = std::memory_order_seq_cst) noexcept {
            return a.compare_exchange_weak(expected, value, success, failure);
        }

        bool compare_exchange_strong(T& expected, const T& value, std::memory_order success = std::memory_order_seq_cst,
                                    std::memory_order failure = std::memory_order_seq_cst) noexcept {
            return a.compare_exchange_strong(expected, value, success, failure);
        }

        bool is_lock_free() const noexcept { return a.is_lock_free(); }

        // Arithmetic / bitwise operations
        // These members are only instantiated when called, so moveable_atomic<T>
        // remains usable for any T; calling one on an unsupported T fails to
        // compile exactly as it would on std::atomic<T> itself.
        T fetch_add(T arg, std::memory_order order = std::memory_order_seq_cst) noexcept { return a.fetch_add(arg, order); }
        T fetch_sub(T arg, std::memory_order order = std::memory_order_seq_cst) noexcept { return a.fetch_sub(arg, order); }
        T fetch_and(T arg, std::memory_order order = std::memory_order_seq_cst) noexcept { return a.fetch_and(arg, order); }
        T fetch_or(T arg, std::memory_order order = std::memory_order_seq_cst) noexcept  { return a.fetch_or(arg, order); }
        T fetch_xor(T arg, std::memory_order order = std::memory_order_seq_cst) noexcept { return a.fetch_xor(arg, order); }

        T operator++() noexcept    { return ++a; }
        T operator++(int) noexcept { return a++; }
        T operator--() noexcept    { return --a; }
        T operator--(int) noexcept { return a--; }

        T operator+=(T arg) noexcept { return a += arg; }
        T operator-=(T arg) noexcept { return a -= arg; }
        T operator&=(T arg) noexcept { return a &= arg; }
        T operator|=(T arg) noexcept { return a |= arg; }
        T operator^=(T arg) noexcept { return a ^= arg; }

#if defined(__cpp_lib_atomic_wait)
        void wait(T old, std::memory_order order = std::memory_order_seq_cst) const noexcept { a.wait(old, order); }
        void notify_one() noexcept { a.notify_one(); }
        void notify_all() noexcept { a.notify_all(); }
#endif

        operator T() const noexcept { return a.load(); }

        operator atomic_type&() noexcept { return a; }

        atomic_type& atomic() noexcept { return a; }
        const atomic_type& atomic() const noexcept { return a; }
    };

    // Moveable atomic flag
    // std::atomic_flag cannot be read without setting it before C++20, so we
    // build on std::atomic<bool> instead - lock free on every platform where
    // std::atomic_flag is, and it gives us copy and move for free.
    struct moveable_atomic_flag {

        std::atomic<bool> a{false};

        moveable_atomic_flag() noexcept = default;
        explicit constexpr moveable_atomic_flag(bool set) noexcept : a(set) {}

        moveable_atomic_flag(const moveable_atomic_flag& cpy) noexcept : a(cpy.a.load()) {}
        moveable_atomic_flag(moveable_atomic_flag&& mve) noexcept : a(mve.a.load()) {}

        moveable_atomic_flag& operator=(const moveable_atomic_flag& other) noexcept {
            a.store(other.a.load());
            return *this;
        }

        moveable_atomic_flag& operator=(moveable_atomic_flag&& other) noexcept {
            a.store(other.a.load());
            return *this;
        }

        bool test_and_set(std::memory_order order = std::memory_order_seq_cst) noexcept { return a.exchange(true, order); }
        void clear(std::memory_order order = std::memory_order_seq_cst) noexcept { a.store(false, order); }
        bool test(std::memory_order order = std::memory_order_seq_cst) const noexcept { return a.load(order); }
    };

    // Aliases
    using moveable_atomic_bool = moveable_atomic<bool>;
    using moveable_atomic_char = moveable_atomic<char>;
    using moveable_atomic_schar = moveable_atomic<signed char>;
    using moveable_atomic_uchar = moveable_atomic<unsigned char>;
    using moveable_atomic_short = moveable_atomic<short>;
    using moveable_atomic_ushort = moveable_atomic<unsigned short>;
    using moveable_atomic_int = moveable_atomic<int>;
    using moveable_atomic_uint = moveable_atomic<unsigned int>;
    using moveable_atomic_long = moveable_atomic<long>;
    using moveable_atomic_ulong = moveable_atomic<unsigned long>;
    using moveable_atomic_llong = moveable_atomic<long long>;
    using moveable_atomic_ullong = moveable_atomic<unsigned long long>;

    using moveable_atomic_wchar_t = moveable_atomic<wchar_t>;
#if defined(__cpp_char8_t)
    using moveable_atomic_char8_t = moveable_atomic<char8_t>;
#endif
    using moveable_atomic_char16_t = moveable_atomic<char16_t>;
    using moveable_atomic_char32_t = moveable_atomic<char32_t>;

    using moveable_atomic_int8_t = moveable_atomic<std::int8_t>;
    using moveable_atomic_uint8_t = moveable_atomic<std::uint8_t>;
    using moveable_atomic_int16_t = moveable_atomic<std::int16_t>;
    using moveable_atomic_uint16_t = moveable_atomic<std::uint16_t>;
    using moveable_atomic_int32_t = moveable_atomic<std::int32_t>;
    using moveable_atomic_uint32_t = moveable_atomic<std::uint32_t>;
    using moveable_atomic_int64_t = moveable_atomic<std::int64_t>;
    using moveable_atomic_uint64_t = moveable_atomic<std::uint64_t>;
    using moveable_atomic_int_least8_t = moveable_atomic<std::int_least8_t>;
    using moveable_atomic_uint_least8_t = moveable_atomic<std::uint_least8_t>;
    using moveable_atomic_int_least16_t = moveable_atomic<std::int_least16_t>;
    using moveable_atomic_uint_least16_t = moveable_atomic<std::uint_least16_t>;
    using moveable_atomic_int_least32_t = moveable_atomic<std::int_least32_t>;
    using moveable_atomic_uint_least32_t = moveable_atomic<std::uint_least32_t>;
    using moveable_atomic_int_least64_t = moveable_atomic<std::int_least64_t>;
    using moveable_atomic_uint_least64_t = moveable_atomic<std::uint_least64_t>;
    using moveable_atomic_int_fast8_t = moveable_atomic<std::int_fast8_t>;
    using moveable_atomic_uint_fast8_t = moveable_atomic<std::uint_fast8_t>;
    using moveable_atomic_int_fast16_t = moveable_atomic<std::int_fast16_t>;
    using moveable_atomic_uint_fast16_t = moveable_atomic<std::uint_fast16_t>;
    using moveable_atomic_int_fast32_t = moveable_atomic<std::int_fast32_t>;
    using moveable_atomic_uint_fast32_t = moveable_atomic<std::uint_fast32_t>;
    using moveable_atomic_int_fast64_t = moveable_atomic<std::int_fast64_t>;
    using moveable_atomic_uint_fast64_t = moveable_atomic<std::uint_fast64_t>;
    using moveable_atomic_intptr_t = moveable_atomic<std::intptr_t>;
    using moveable_atomic_uintptr_t = moveable_atomic<std::uintptr_t>;
    using moveable_atomic_size_t = moveable_atomic<std::size_t>;
    using moveable_atomic_ptrdiff_t = moveable_atomic<std::ptrdiff_t>;
    using moveable_atomic_intmax_t = moveable_atomic<std::intmax_t>;
    using moveable_atomic_uintmax_t = moveable_atomic<std::uintmax_t>;
} // namespace snicholls

#endif /* moveable_atomic_hpp */
