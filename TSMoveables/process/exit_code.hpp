//
//  exit_code.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - what a process's exit status means, so a
//  supervisor can tell "restart me" from "fix the config first":
//
//      return snicholls::to_int(snicholls::exit_code::config);   // 78: systemd should not restart this
//
//  The values are the BSD sysexits ones where one fits, 124 as timeout(1)
//  uses it, and 128 + n for "killed by signal n", as shells report it. For
//  systemd, `RestartPreventExitStatus=64 78` stops a restart loop on a bad
//  command line or configuration, which no restart can fix.
//
//  Ported from an earlier application framework's exit_code.hpp. C++17, standard library only.
//

#ifndef ts_moveables_process_exit_code_hpp
#define ts_moveables_process_exit_code_hpp

#include <string_view>

namespace snicholls {

enum class exit_code : int
{
    success = 0,
    failure = 1,
    usage = 64,               // EX_USAGE: bad command line
    unavailable = 69,         // EX_UNAVAILABLE: a required service is missing
    software = 70,            // EX_SOFTWARE: an unhandled exception, an internal error
    temporary_failure = 75,   // EX_TEMPFAIL: a supervised worker gave up; restart the process
    config = 78,              // EX_CONFIG: invalid configuration
    shutdown_timeout = 124,   // graceful shutdown overran its deadline
};

[[nodiscard]] constexpr int to_int(exit_code code) noexcept { return static_cast<int>(code); }

// The status a shell reports for a process killed by `signal_number`
[[nodiscard]] constexpr int signal_exit_code(int signal_number) noexcept { return 128 + signal_number; }

[[nodiscard]] constexpr std::string_view describe(exit_code code) noexcept
{
    switch (code) {
    case exit_code::success: return "success";
    case exit_code::failure: return "failure";
    case exit_code::usage: return "usage error";
    case exit_code::unavailable: return "service unavailable";
    case exit_code::software: return "internal software error";
    case exit_code::temporary_failure: return "temporary failure (restart)";
    case exit_code::config: return "configuration error";
    case exit_code::shutdown_timeout: return "shutdown timed out";
    }
    return "unknown";
}

} // namespace snicholls

#endif // ts_moveables_process_exit_code_hpp
