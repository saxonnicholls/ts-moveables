//
//  utils/time.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - wall-clock helpers shared across the library.
//
//  Wall clock, deliberately: `unix_millis` is for timestamps that leave the
//  process - a JSON frame sent to a browser, a line in a flight record, an
//  event id another machine will sort by. It is NOT for measuring elapsed
//  time. `system_clock` can jump backwards when NTP corrects it, so a duration
//  taken from two of these can be negative. Use `steady_clock` for anything
//  you intend to subtract, which is what the benchmarks and timeouts do.
//

#ifndef ts_moveables_utils_time_hpp
#define ts_moveables_utils_time_hpp

#include <chrono>
#include <cstdint>

namespace snicholls {
namespace utils {

// Milliseconds since the Unix epoch, as the wire and the browser expect it.
inline std::uint64_t unix_millis() noexcept
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

} // namespace utils
} // namespace snicholls

#endif /* ts_moveables_utils_time_hpp */
