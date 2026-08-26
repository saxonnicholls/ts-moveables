//
//  utils/cpu_relax.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - the hint a spin loop owes the processor.
//
//  A bare `while (!ready()) {}` is not free just because it does no work. On a
//  superscalar core it fills the pipeline with speculative loads of the very
//  location another core is about to write, and when the write lands the
//  machine has to discard that work - a memory-order violation, which costs
//  tens of cycles precisely at the moment the spinner was about to succeed.
//  It also burns power at full rate and, on SMT, starves the sibling thread
//  sharing the execution units.
//
//  `PAUSE` on x86 and `YIELD` on AArch64 say "I am spinning": drain the
//  speculative loads, drop to a lower issue rate, let the sibling have the
//  pipeline. They are hints, not barriers - no ordering is implied and none is
//  wanted, because the atomic load in the loop already carries it.
//
//  This was missing from every spin path here - the spin lock, and both the
//  busy-spin and yielding disruptor wait strategies - which an outside reviewer
//  evaluating the library for a latency-sensitive system noticed and we had
//  not. It matters most in exactly the case the busy-spin strategy exists to
//  serve: a consumer spinning on a producer's cursor on a neighbouring core.
//
//  Deliberately no <immintrin.h> on GCC/Clang: __builtin_ia32_pause() is the
//  same instruction without dragging a large intrinsics header into a
//  dependency-free library.
//

#ifndef ts_moveables_utils_cpu_relax_hpp
#define ts_moveables_utils_cpu_relax_hpp

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace snicholls {
namespace utils {

// One iteration's worth of "I am spinning, not working". Never a barrier.
inline void cpu_relax() noexcept
{
#if defined(_MSC_VER)
#if defined(_M_ARM64) || defined(_M_ARM)
    __yield();
#else
    _mm_pause();
#endif
#elif defined(__i386__) || defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    // Any other architecture: the loop is still correct, just less polite.
#endif
}

} // namespace utils
} // namespace snicholls

#endif /* ts_moveables_utils_cpu_relax_hpp */
