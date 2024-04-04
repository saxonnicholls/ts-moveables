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

namespace snicholls
{
    // Moveable atomic
    // We use composition here rather than inheritance
    template <typename T>
    struct moveable_atomic {
        
        using atomic_type = std::atomic<T>;
        atomic_type a;                          // a for atomic
        
        moveable_atomic() : a() {}
        moveable_atomic(T t) : a(t){}
        explicit moveable_atomic(const std::atomic<T>& a)       : a(a.load()) {}
        explicit moveable_atomic(const moveable_atomic& cpy)    : a(cpy.a.load()) {}
        explicit moveable_atomic(moveable_atomic&& mve)         : a(mve.a.load()) {}
        
        moveable_atomic& operator=(const moveable_atomic& other) {
            a.store(other.a.load());
            return *this;
        }
        
        moveable_atomic& operator=(const T& other) {
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
        
        operator T() const noexcept { return a.load(); }
        
        operator atomic_type&() noexcept { return a; }
        
        atomic_type& atomic() noexcept { return a; }
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

// If we want them... C++20 onwards
//    using moveable_atomic_char8_t = moveable_atomic<char8_t>;
//    using moveable_atomic_char16_t = moveable_atomic<char16_t>;
//    using moveable_atomic_char32_t = moveable_atomic<char32_t>;
//    using moveable_atomic_wchar_t = moveable_atomic<wchar_t>;

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
    using moveable_atomic_int_fast32_ = moveable_atomic<std::int_fast32_t>;
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
