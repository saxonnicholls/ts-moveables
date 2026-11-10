//
//  remote/kernel.hpp
//  TSMoveables - demos/remote_compute (from the remote_compute_example prototype at 92d8dc4)
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Kernels: the named functions a remote machine may be asked to run, and the
//  compile-time dispatch table that finds one from the name in a frame.
//
//      double price(const option_spec&, int);
//      std::uint64_t count_primes(std::uint64_t, std::uint64_t);
//
//      using kernels = snicholls::remote::kernel_set<
//          snicholls::remote::kernel<"price", &price>,
//          snicholls::remote::kernel<"count_primes", &count_primes>>;
//
//  Both ends compile the same list. A machine can only ever run what it was
//  built with: no code crosses the wire, only a name, a signature hash and
//  arguments.
//
//  -------------------------------------------------------------- dispatch
//
//  The list is known at compile time, so the name lookup is a perfect hash
//  generated at compile time: the seed below is searched for in a constant
//  expression until every name lands in its own slot, and a list for which no
//  seed is found fails to compile rather than collide at run time. A lookup is
//  one FNV-1a pass over the name, a mask, one string comparison and an index
//  into an array of function pointers - no map, no allocation, no branch per
//  kernel. The idea is ConstexprCore/perfect_hash's (MIT); that library needs
//  C++23, which the Raspberry Pis' GCC 12 does not have, so this is a minimal
//  C++20 version of the same technique over core's fixed_string.
//
//  On the calling side, a kernel is found from its function pointer with a
//  compile-time-unrolled comparison (core's constexpr_for): one compare per
//  kernel, no hashing at all.
//

#ifndef remote_kernel_hpp
#define remote_kernel_hpp

#include "wire.hpp"

#include <snicholls/core/constexpr_for.hpp>
#include <snicholls/core/fixed_string.hpp>
#include <snicholls/core/meta/callable_traits.hpp>
#include <snicholls/core/meta/type_list.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace snicholls::remote {

namespace detail {

template <typename R, typename Arguments>
struct wire_signature;
template <typename R, typename... A>
struct wire_signature<R, meta::type_list<A...>> {
    static constexpr std::uint64_t value = function_signature_v<R, A...>;
};

// What the wire needs of a kernel's function, on top of snicholls-core's
// callable_traits (which already handles plain and noexcept function
// pointers): the arguments as values to decode into, and the signature hash
template <typename F>
struct function_traits {
    using result = callable_result_t<F>;
    using args = meta::rename_t<meta::transform_t<callable_arguments_t<F>, std::remove_cvref_t>, std::tuple>;
    static constexpr std::uint64_t signature = wire_signature<result, callable_arguments_t<F>>::value;
};

constexpr std::uint64_t name_hash(std::string_view s, std::uint64_t seed) noexcept
{
    std::uint64_t h = 0xcbf29ce484222325ull ^ (seed * 0x9e3779b97f4a7c15ull);
    for (const char c : s) {
        h ^= static_cast<unsigned char>(c);
        h *= 0x100000001b3ull;
    }
    return h ^ (h >> 29);
}

constexpr std::size_t next_pow2(std::size_t n) noexcept
{
    std::size_t p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

} // namespace detail

/// One kernel: a name both ends agree on, and the function it runs.
template <snicholls::basic_fixed_string Name, auto Fn>
struct kernel {
    static constexpr std::string_view name = Name;
    static constexpr auto function = Fn;
    using traits = detail::function_traits<decltype(Fn)>;
    using result = typename traits::result;
    static constexpr std::uint64_t signature = traits::signature;

    static_assert(!name.empty() && name.size() <= 100, "kernel names are 1 to 100 characters");

    /// Decode the arguments, run the function, encode the result.
    static void invoke(std::string_view args, std::string& out)
    {
        typename traits::args values;
        reader r(args);
        std::apply([&r](auto&... v) { (r.get(v), ...); }, values);
        r.expect_end();
        if constexpr (std::is_void_v<result>)
            std::apply(Fn, std::move(values));
        else
            encode(out, std::apply(Fn, std::move(values)));
    }
};

/// The fixed set of kernels a cluster understands, with its compile-time dispatch table.
template <typename... Kernels>
struct kernel_set {
    static constexpr std::size_t size = sizeof...(Kernels);
    static_assert(size > 0, "a kernel_set needs at least one kernel");

    static constexpr std::array<std::string_view, size> names{Kernels::name...};
    static constexpr std::array<std::uint64_t, size> signatures{Kernels::signature...};

    using invoker = void (*)(std::string_view, std::string&);
    static constexpr std::array<invoker, size> invokers{&Kernels::invoke...};

    static constexpr std::size_t slots = detail::next_pow2(size * 2);

private:
    struct table {
        std::uint64_t seed = 0;
        std::array<std::uint16_t, slots> slot{};   // index + 1; 0 means empty
    };

    static constexpr table build()
    {
        for (std::size_t i = 0; i < size; ++i)
            for (std::size_t j = i + 1; j < size; ++j)
                if (names[i] == names[j])
                    throw "kernel_set: two kernels share a name";   // a compile error in a constant expression
        for (std::uint64_t seed = 0; seed < 100000; ++seed) {
            table t{seed, {}};
            bool ok = true;
            for (std::size_t i = 0; i < size && ok; ++i) {
                const auto s = detail::name_hash(names[i], seed) & (slots - 1);
                if (t.slot[s] != 0)
                    ok = false;
                else
                    t.slot[s] = static_cast<std::uint16_t>(i + 1);
            }
            if (ok)
                return t;
        }
        throw "kernel_set: no perfect hash seed found";
    }

    static constexpr table table_ = build();

public:
    static constexpr std::uint64_t seed = table_.seed;

    /// The index of the kernel called `name`, or -1. One hash, one compare.
    static constexpr int find(std::string_view name) noexcept
    {
        const auto s = table_.slot[detail::name_hash(name, seed) & (slots - 1)];
        return s != 0 && names[s - 1] == name ? int(s) - 1 : -1;
    }

    /// The index of the kernel whose function is `fn`, or -1. Unrolled at compile time.
    template <typename F>
    static int index_of(F* fn) noexcept
    {
        int found = -1;
        int i = 0;
        snicholls::constexpr_for<size>([&](auto I) {
            using K = std::tuple_element_t<I.value, std::tuple<Kernels...>>;
            if constexpr (std::is_same_v<std::remove_const_t<decltype(K::function)>, F*>)
                if (K::function == fn)
                    found = i;
            ++i;
        });
        return found;
    }
};

} // namespace snicholls::remote

#endif /* remote_kernel_hpp */
