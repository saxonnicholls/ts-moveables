//
//  health_test.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - process/health.hpp tests, ported from an earlier application framework's health tests.
//

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#include "mini_gtest.hpp"

#include "process/health.hpp"

namespace {

using namespace std::chrono_literals;
using snicholls::health_state;

TEST(AppHealth, AggregationWorstWins)
{
    snicholls::health_registry h;
    EXPECT_EQ(h.report().overall, health_state::ready);   // nothing registered
    h.set("db", health_state::ready);
    h.set("cache", health_state::degraded, "warming");
    auto r = h.report();
    EXPECT_EQ(r.overall, health_state::degraded);
    EXPECT_TRUE(r.live());
    EXPECT_TRUE(r.ready());
    h.set("feed", health_state::starting);
    EXPECT_FALSE(h.report().ready());
    h.set("feed", health_state::unhealthy);
    EXPECT_FALSE(h.report().live());
    h.remove("feed");
    h.set_stopping(true);
    EXPECT_EQ(h.report().overall, health_state::stopping);
    EXPECT_FALSE(h.report().ready());
}

TEST(AppHealth, ProbesEvaluatedAtReportTimeAndThrowingProbeIsUnhealthy)
{
    snicholls::health_registry h;
    int calls = 0;
    h.add_probe("p", [&] {
        ++calls;
        return std::vector<snicholls::component_health>{{"p.x", health_state::ready, ""}};
    });
    h.add_probe("broken", []() -> std::vector<snicholls::component_health> { throw std::runtime_error("down"); });
    h.add_probe("weird", []() -> std::vector<snicholls::component_health> { throw 1; });
    const auto r = h.report();
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(r.overall, health_state::unhealthy);
    ASSERT_EQ(r.components.size(), 3u);
    h.remove_probe("broken");
    h.remove_probe("weird");
    EXPECT_EQ(h.report().overall, health_state::ready);
}

TEST(AppHealth, JsonReport)
{
    snicholls::health_report r;
    r.overall = health_state::degraded;
    r.components.push_back({"a\"b", health_state::degraded, "line\nbreak"});
    EXPECT_EQ(r.to_json(),
              R"({"status":"degraded","live":true,"ready":true,"components":[{"name":"a\"b","status":"degraded","detail":"line\nbreak"}]})");
    for (const health_state s : {health_state::ready, health_state::degraded, health_state::starting, health_state::unhealthy,
                                 health_state::stopping})
        EXPECT_NE(snicholls::to_string(s), "?");
}

TEST(AppHealth, HealthFileIsWrittenAtomicallyAndRemoved)
{
    const auto path = std::filesystem::temp_directory_path() / ("tsm_process_health_" + std::to_string(::getpid()) + ".json");
    snicholls::health_file f(path);
    snicholls::health_report r;
    EXPECT_TRUE(f.write(r));
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    EXPECT_EQ(text.str(), r.to_json() + "\n");
    EXPECT_FALSE(std::filesystem::exists(path.string() + ".tmp"));
    f.remove();
    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_EQ(f.path(), path);
    snicholls::health_file bad("/nonexistent-dir/tsm/health.json");
    EXPECT_FALSE(bad.write(r));
}

TEST(AppServiceNotifier, DisabledWithoutSocket)
{
    const snicholls::service_notifier n;
    EXPECT_FALSE(n.enabled());
    EXPECT_FALSE(n.ready());
    EXPECT_FALSE(n.status("x"));
    EXPECT_EQ(snicholls::service_notifier::from_environment({}).enabled(), false);
}

TEST(AppServiceNotifier, SendsDatagramsToNotifySocket)
{
    const std::string path = "/tmp/tsm_notify_" + std::to_string(::getpid());
    ::unlink(path.c_str());
    const int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    ASSERT_GE(fd, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    ASSERT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof address), 0);
    timeval tv{2, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    const auto n = snicholls::service_notifier::from_environment(
        {{"NOTIFY_SOCKET", path}, {"WATCHDOG_USEC", "2000000"}, {"WATCHDOG_PID", std::to_string(::getpid())}});
    EXPECT_TRUE(n.enabled());
    EXPECT_EQ(n.watchdog_interval(), 2s);
    const auto receive = [&] {
        char buffer[256];
        const ::ssize_t got = ::recv(fd, buffer, sizeof buffer, 0);
        return got > 0 ? std::string(buffer, static_cast<std::size_t>(got)) : std::string();
    };
    EXPECT_TRUE(n.ready());
    EXPECT_EQ(receive(), "READY=1");
    EXPECT_TRUE(n.watchdog());
    EXPECT_EQ(receive(), "WATCHDOG=1");
    EXPECT_TRUE(n.stopping());
    EXPECT_EQ(receive(), "STOPPING=1");
    EXPECT_TRUE(n.status("warming up"));
    EXPECT_EQ(receive(), "STATUS=warming up");
    EXPECT_TRUE(n.reloading());
    EXPECT_EQ(receive().rfind("RELOADING=1\nMONOTONIC_USEC=", 0), 0u);
    ::close(fd);
    ::unlink(path.c_str());

    // Watchdog meant for another process is ignored.
    EXPECT_EQ(snicholls::service_notifier::from_environment({{"WATCHDOG_USEC", "5"}, {"WATCHDOG_PID", "1"}}).watchdog_interval(),
              std::chrono::microseconds(0));
    EXPECT_FALSE(snicholls::service_notifier(std::string(200, 'x')).notify("X"));   // path too long
}

} // namespace
