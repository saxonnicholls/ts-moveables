//
//  constexpr_for.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - compile-time loop unrolling
//
//  DEPRECATED here: constexpr_for, constexpr_nest and unrolled_for live in snicholls-core,
//  <snicholls/core/constexpr_for.hpp>, and this header only forwards to it, so the two can be included together
//  (both used to define snicholls::constexpr_for). Include core's header in new code. Core keeps that header
//  C++17-clean for ts-moveables; the amalgamated single headers carry it inline.
//
//      constexpr_for<0, 4>      ([&](auto I) { use<I.value>(); });   // fully unrolled
//      constexpr_for<0, 10, 2>  ([&](auto I) { even(I); });          // with a step
//      constexpr_for<10, 0, -1> ([&](auto I) { down(I); });          // and backwards
//      constexpr_for<0, 3>      (f, a, b);                           // f(I, a, b)
//      unrolled_for<4>(0, n, [&](std::size_t i) { out[i] = g(in[i]); });
//
//  constexpr_for<Start, End, Inc> expands into exactly the calls it needs and
//  leaves no loop at all - the bounds are template parameters, so there is no
//  counter, no compare and no branch. unrolled_for takes a RUN-TIME range and
//  emits the body Unroll times per iteration, with a tail for the remainder.
//
//  ------------------------------------------------------ the index is a type
//
//  The body is handed std::integral_constant<T, I>, not a bare T, and that is
//  the whole reason this is worth having over a plain loop.
//
//  A function parameter is never a constant expression, however constant the
//  value passed in was - so a body taking `std::size_t i` cannot write
//  std::get<i>(tuple) or use i as a template argument, which is usually the
//  entire point of unrolling by hand. Handing it a type carrying the index
//  fixes that: `I.value` is a constant expression inside the body, while `I`
//  still converts implicitly to its underlying type, so the ordinary
//  arithmetic spelling keeps working:
//
//      constexpr_for<0, N>([&](auto I) { sum += v[I]; });                // fine
//      constexpr_for<0, N>([&](auto I) { use(std::get<I.value>(tup)); }); // also fine
//
//  The index type is decltype(Start), so constexpr_for<0, 4> yields int and
//  constexpr_for<std::size_t{0}, std::size_t{4}> yields std::size_t. Say which
//  you want when it matters.
//
//  ------------------------------------------------------------ expansion, not
//                                                                  recursion
//  The obvious implementation recurses - body, then constexpr_for<Start+Inc,
//  End, Inc> - and it is shorter. This expands an index_sequence through a fold
//  instead, because the recursive form instantiates one template per iteration:
//  template depth grows with the count, a few hundred iterations meets the
//  compiler's instantiation-depth limit, and compile time grows superlinearly
//  well before that. The fold is depth 1 whatever the count. Same unrolled
//  output, no cliff to fall off.
//
//  ------------------------------------------------------------ extra arguments
//
//  Trailing arguments are passed through to every call as LVALUES, never
//  forwarded. Forwarding would move the same object once per iteration, which
//  is a use-after-move that compiles quietly; if a body needs to consume
//  something it should capture it instead.
//
//  ------------------------------------------------------------- when to reach
//
//  Be sceptical of unrolled_for as a speed tool. At -O2 the compiler already
//  unrolls simple counted loops, and doing it by hand can lose: more code in
//  the instruction cache, more live registers, and a tail that confuses the
//  vectoriser. `make bench-parallel` prints unrolled against not so the claim
//  stays falsifiable rather than folkloric.
//
//  Where it does earn its place: when the body must be instantiated per index -
//  a tuple element, a template argument, a fixed-size matrix row - which no
//  compiler unroll can do for you, because that is a language requirement
//  rather than an optimisation. Reach for constexpr_for for that; reach for
//  unrolled_for only with a benchmark in hand.
//

#ifndef constexpr_for_hpp
#define constexpr_for_hpp

#include <snicholls/core/constexpr_for.hpp>

#endif /* constexpr_for_hpp */
