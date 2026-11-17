//
//  exit_code_test.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - process/exit_code.hpp tests, ported from an earlier application framework's exit_code tests.
//

#include <csignal>

#include "mini_gtest.hpp"

#include "process/config.hpp"
#include "process/global.hpp"
#include "process/exit_code.hpp"

namespace {

using snicholls::exit_code;

TEST(AppExitCode, ValuesFollowSysexitsAndConventions)
{
    static_assert(snicholls::to_int(exit_code::success) == 0);
    static_assert(snicholls::to_int(exit_code::usage) == 64);
    static_assert(snicholls::to_int(exit_code::unavailable) == 69);
    static_assert(snicholls::to_int(exit_code::software) == 70);
    static_assert(snicholls::to_int(exit_code::temporary_failure) == 75);
    static_assert(snicholls::to_int(exit_code::config) == 78);
    static_assert(snicholls::to_int(exit_code::shutdown_timeout) == 124);
    static_assert(snicholls::signal_exit_code(SIGINT) == 130);
    static_assert(snicholls::signal_exit_code(SIGTERM) == 143);
    static_assert(noexcept(snicholls::to_int(exit_code::success)));
}

TEST(AppExitCode, EveryCodeHasADescription)
{
    for (const exit_code c : {exit_code::success, exit_code::failure, exit_code::usage, exit_code::unavailable,
                              exit_code::software, exit_code::temporary_failure, exit_code::config, exit_code::shutdown_timeout})
        EXPECT_NE(snicholls::describe(c), "unknown");
    EXPECT_EQ(snicholls::describe(static_cast<exit_code>(42)), "unknown");
}

TEST(AppErrors, ErrorTypesDeriveFromRuntimeError)
{
    EXPECT_THROW(throw snicholls::config_error("c"), std::runtime_error);
    EXPECT_THROW(throw snicholls::global_init_error("c"), std::runtime_error);
}

} // namespace
