//
//  signals_test.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - process/signals.hpp tests, ported from an earlier application framework's signals tests.
//

#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "mini_gtest.hpp"

#include "process/signals.hpp"

namespace {

using namespace std::chrono_literals;

// Collects delivered signals from the watcher thread.
struct received
{
    std::mutex m;
    std::condition_variable cv;
    std::vector<int> signals;

    void add(int sig)
    {
        {
            const std::lock_guard lock(m);
            signals.push_back(sig);
        }
        cv.notify_all();
    }

    bool wait_for_count(std::size_t n)
    {
        std::unique_lock lock(m);
        return cv.wait_for(lock, 5s, [&] { return signals.size() >= n; });
    }
};

TEST(AppSignals, RaisedSignalsReachTheCallbackOnTheWatcherThread)
{
    received r;
    std::thread::id callback_thread;
    {
        snicholls::signal_watcher watcher([&](int sig) {
            callback_thread = std::this_thread::get_id();
            r.add(sig);
        });
        ::raise(SIGHUP);
        ::raise(SIGUSR1);
        ::raise(SIGUSR2);
        ASSERT_TRUE(r.wait_for_count(3));
        EXPECT_EQ(watcher.handled(), 3);
    }
    EXPECT_EQ(r.signals, (std::vector<int>{SIGHUP, SIGUSR1, SIGUSR2}));
    EXPECT_NE(callback_thread, std::this_thread::get_id());
}

TEST(AppSignals, FirstTerminationSignalIsGraceful)
{
    received r;
    snicholls::signal_watcher watcher([&](int sig) { r.add(sig); });
    ::raise(SIGTERM);
    ASSERT_TRUE(r.wait_for_count(1));
    EXPECT_EQ(r.signals[0], SIGTERM);
    snicholls::signal_watcher::reset_termination_count();
    ::raise(SIGINT);   // graceful again after the reset
    ASSERT_TRUE(r.wait_for_count(2));
}

TEST(AppSignals, PreviousHandlersAreRestored)
{
    static std::atomic<int> previous_calls{0};
    previous_calls = 0;
    struct sigaction mine{};
    mine.sa_handler = [](int) { ++previous_calls; };
    sigemptyset(&mine.sa_mask);
    struct sigaction saved{};
    ASSERT_EQ(::sigaction(SIGUSR1, &mine, &saved), 0);
    {
        snicholls::signal_watcher watcher([](int) {});
    }
    ::raise(SIGUSR1);
    EXPECT_EQ(previous_calls.load(), 1);
    ::sigaction(SIGUSR1, &saved, nullptr);
}

TEST(AppSignals, SigpipeIsIgnoredWhileWatching)
{
    snicholls::signal_watcher watcher([](int) {});
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);
    ::close(fds[0]);
    const char byte = 'x';
    EXPECT_EQ(::write(fds[1], &byte, 1), -1);   // EPIPE, not death by SIGPIPE
    EXPECT_EQ(errno, EPIPE);
    ::close(fds[1]);
}

TEST(AppSignals, OnlyOneWatcherAtATime)
{
    snicholls::signal_watcher watcher([](int) {});
    EXPECT_THROW(snicholls::signal_watcher([](int) {}), std::logic_error);
}

TEST(AppSignals, ThrowingCallbackIsContained)
{
    received r;
    snicholls::signal_watcher watcher([&](int sig) {
        r.add(sig);
        throw std::runtime_error("callback failed");
    });
    ::raise(SIGUSR2);
    ::raise(SIGUSR2);
    EXPECT_TRUE(r.wait_for_count(2));
}

TEST(AppSignals, SignalNames)
{
    EXPECT_EQ(snicholls::signal_name(SIGTERM), "SIGTERM");
    EXPECT_EQ(snicholls::signal_name(SIGSEGV), "SIGSEGV");
    EXPECT_EQ(snicholls::signal_name(1000), "signal 1000");
}

TEST(AppSignals, FormatIntIsCorrect)
{
    char buffer[16];
    EXPECT_EQ(std::string(buffer, snicholls::detail::sig::format_int(buffer, 143)), "143");
    EXPECT_EQ(std::string(buffer, snicholls::detail::sig::format_int(buffer, 0)), "0");
    EXPECT_EQ(std::string(buffer, snicholls::detail::sig::format_int(buffer, -7)), "-7");
}

using AppSignalsDeathTest = testing::Test;

TEST_F(AppSignalsDeathTest, SecondTerminationSignalExitsImmediately)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            snicholls::signal_watcher watcher([](int) { std::this_thread::sleep_for(10s); });   // stuck shutdown
            ::raise(SIGTERM);
            std::this_thread::sleep_for(100ms);
            ::raise(SIGTERM);
            std::this_thread::sleep_for(5s);
            std::_Exit(0);
        },
        testing::ExitedWithCode(143), "second termination signal");
}

TEST_F(AppSignalsDeathTest, CrashHandlerReportsAndKeepsTheSignalStatus)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            snicholls::install_crash_handlers();
            snicholls::set_crash_hook(+[](int) noexcept { snicholls::detail::sig::write_stderr("crash hook ran\n"); });
            ::raise(SIGSEGV);
        },
        testing::KilledBySignal(SIGSEGV), "fatal signal SIGSEGV.*\n.*crash hook ran");
}

TEST_F(AppSignalsDeathTest, TerminateHandlerLogsTheException)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            snicholls::install_crash_handlers();
            EXPECT_FALSE(snicholls::install_crash_handlers());   // idempotent
            std::thread([] { throw std::runtime_error("escaped a thread"); }).join();
        },
        "uncaught exception: escaped a thread");
}

} // namespace
