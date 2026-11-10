//
//  kernels.hpp
//  TSMoveables - demos/remote_compute (from the remote_compute_example prototype at 92d8dc4)
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  The kernels this example's machines can run. Every machine compiles this
//  file; the list at the bottom is the whole contract between them.
//

#ifndef remote_example_kernels_hpp
#define remote_example_kernels_hpp

#include "remote/cluster.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>

namespace example {

struct option_spec {
    double spot = 100;
    double strike = 100;
    double rate = 0.05;
    double vol = 0.2;
    double years = 1;
};

// A chunk of a Monte Carlo price. Chunks combine by adding, so a big job splits
// across machines and the sums come back in chunk order, bit for bit the same
// as one machine doing it all.
struct mc_chunk {
    double sum = 0;
    double sum_sq = 0;
    std::uint64_t paths = 0;
};

// European call by Monte Carlo. Deterministic across machines: mt19937_64's
// output is fixed by the standard, but std::normal_distribution is not - libc++
// on a Mac and libstdc++ on a Pi give different normals from the same engine -
// so the normals come from Box-Muller here, written out.
inline mc_chunk price_chunk(option_spec o, std::uint64_t seed, std::uint64_t paths)
{
    std::mt19937_64 rng(seed);
    constexpr double two_pi = 6.283185307179586476925;
    const double drift = (o.rate - 0.5 * o.vol * o.vol) * o.years;
    const double diffusion = o.vol * std::sqrt(o.years);
    const double discount = std::exp(-o.rate * o.years);
    mc_chunk out;
    for (std::uint64_t i = 0; i < paths; ++i) {
        const double u1 = (static_cast<double>(rng() >> 11) + 0.5) * 0x1.0p-53;
        const double u2 = static_cast<double>(rng() >> 11) * 0x1.0p-53;
        const double z = std::sqrt(-2.0 * std::log(u1)) * std::cos(two_pi * u2);
        const double payoff = std::max(o.spot * std::exp(drift + diffusion * z) - o.strike, 0.0) * discount;
        out.sum += payoff;
        out.sum_sq += payoff * payoff;
    }
    out.paths = paths;
    return out;
}

inline std::uint64_t count_primes(std::uint64_t lo, std::uint64_t hi)
{
    std::uint64_t n = 0;
    for (std::uint64_t x = lo < 2 ? 2 : lo; x < hi; ++x) {
        bool prime = true;
        for (std::uint64_t d = 2; d * d <= x; ++d)
            if (x % d == 0) {
                prime = false;
                break;
            }
        n += prime;
    }
    return n;
}

// Which machine ran this, and what it is
inline snicholls::remote::node_info whoami()
{
    return {snicholls::remote::this_node::name(), std::thread::hardware_concurrency(),
            snicholls::remote::detail::arch_name(), snicholls::remote::detail::host_name()};
}

inline void must_fail(std::string why) { throw std::runtime_error(why); }

inline void sleep_ms(std::uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

inline std::vector<double> scale(std::vector<double> v, double k)
{
    for (auto& x : v)
        x *= k;
    return v;
}

// A long job: the same Monte Carlo, in blocks, reporting progress and
// stopping early if cancelled. Blocks reuse price_chunk with per-block seeds,
// so the answer is the same whichever machine runs it.
inline mc_chunk long_price(option_spec o, std::uint64_t paths)
{
    namespace job = snicholls::remote::this_job;
    constexpr std::uint64_t block = 200'000;
    mc_chunk total;
    for (std::uint64_t done = 0, b = 0; done < paths; done += block, ++b) {
        job::throw_if_cancelled();
        const auto part = price_chunk(o, 5000 + b, std::min(block, paths - done));
        total.sum += part.sum;
        total.sum_sq += part.sum_sq;
        total.paths += part.paths;
        job::progress(double(total.paths) / double(paths),
                      std::to_string(total.paths / 1'000'000) + "M of " + std::to_string(paths / 1'000'000) +
                          "M paths, price so far " + std::to_string(total.sum / double(total.paths)));
    }
    return total;
}

// For measuring the cost of the call itself: no work, a little work, and
// payloads of any size in both directions
inline void noop() {}
inline std::int64_t add(std::int64_t a, std::int64_t b) { return a + b; }
inline std::string echo(std::string s) { return s; }

namespace r = snicholls::remote;

using kernels = r::kernel_set<
    r::kernel<"price_chunk", &price_chunk>,
    r::kernel<"count_primes", &count_primes>,
    r::kernel<"whoami", &whoami>,
    r::kernel<"must_fail", &must_fail>,
    r::kernel<"sleep_ms", &sleep_ms>,
    r::kernel<"scale", &scale>,
    r::kernel<"noop", &noop>,
    r::kernel<"add", &add>,
    r::kernel<"echo", &echo>,
    r::kernel<"long_price", &long_price>>;

} // namespace example

#endif /* remote_example_kernels_hpp */
