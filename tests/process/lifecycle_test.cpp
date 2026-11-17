//
//  lifecycle_test.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - tests for what process/ adds to the copies it was ported from: the run-time environment, ${NAME:?},
//  the boot gate, clock_check, boot_monitor on an event loop, signals delivered on an event loop,
//  scoped_ignore_sigpipe, and /healthz and /readyz on this library's HTTP server.
//

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "mini_gtest.hpp"

#include "process/dotenv.hpp"
#include "process/environment.hpp"
#include "process/health.hpp"
#include "process/http_health.hpp"
#include "process/signals.hpp"

namespace {

using namespace std::chrono_literals;
using snicholls::check_result;
using snicholls::check_severity;
using snicholls::run_environment;

snicholls::config config_from(std::map<std::string, std::string> env)
{
    snicholls::config_sources s;
    s.env_files = {};
    s.environment = std::move(env);
    return snicholls::load_config(s);
}

// ------------------------------------------------------------------ environment

TEST(Environment, ParsesTheUsualSpellings)
{
    EXPECT_EQ(snicholls::parse_run_environment("Production"), run_environment::production);
    EXPECT_EQ(snicholls::parse_run_environment("prod"), run_environment::production);
    EXPECT_EQ(snicholls::parse_run_environment(" STAGING "), run_environment::staging);
    EXPECT_EQ(snicholls::parse_run_environment("dev"), run_environment::local);
    EXPECT_FALSE(snicholls::parse_run_environment("prodution").has_value());
    EXPECT_TRUE(snicholls::is_strict(run_environment::staging));
    EXPECT_FALSE(snicholls::is_strict(run_environment::local));
}

TEST(Environment, UnsetIsTheCallersFallbackUnknownIsAnError)
{
    EXPECT_EQ(*snicholls::environment_or(config_from({}), run_environment::staging), run_environment::staging);
    EXPECT_EQ(*snicholls::environment_or(config_from({{"ENVIRONMENT", ""}}), run_environment::local),
              run_environment::local);
    EXPECT_EQ(*snicholls::environment_or(config_from({{"ENVIRONMENT", "production"}}), run_environment::local),
              run_environment::production);
    const auto typo = snicholls::environment_or(config_from({{"ENVIRONMENT", "prodution"}}), run_environment::local);
    ASSERT_TRUE(typo.is_err());
    EXPECT_NE(std::string(typo.unwrap_err_unchecked().what()).find("prodution"), std::string::npos);
}

TEST(Environment, AValueInADotenvFileCounts)
{
    const auto path = std::filesystem::temp_directory_path() / ("tsm_env_" + std::to_string(::getpid()) + ".env");
    ASSERT_TRUE(snicholls::write_file_atomically(path, "ENVIRONMENT=staging\n", {.durable = false}).is_ok());
    snicholls::config_sources s;
    s.env_files = {path};
    s.environment = std::map<std::string, std::string>{};
    const auto cfg = snicholls::load_config(s);
    std::filesystem::remove(path);
    EXPECT_EQ(*snicholls::environment_or(cfg, run_environment::local), run_environment::staging);
}

// ------------------------------------------------------------------ ${NAME:?message}

TEST(Dotenv, RequiredInterpolationFailsWhenUnsetOrEmpty)
{
    const auto lookup = [](std::string_view name) -> std::optional<std::string> {
        if (name == "HOME") return std::string("/home/me");
        if (name == "BLANK") return std::string();
        return std::nullopt;
    };
    const auto ok = snicholls::parse_dotenv("A=${HOME:?set HOME}/x\n", "t.env", lookup);
    EXPECT_EQ(ok.at(0).value, "/home/me/x");
    EXPECT_EQ(snicholls::parse_dotenv("B=1\nC=${B:?}\n", "t.env").at(1).value, "1");
    for (const char* text : {"A=${MISSING:?set MISSING first}\n", "A=\"${BLANK:?must not be empty}\"\n"}) {
        try {
            static_cast<void>(snicholls::parse_dotenv(text, "t.env", lookup));
            ADD_FAILURE() << "no error for " << text;
        } catch (const snicholls::config_error& e) {
            const std::string what = e.what();
            EXPECT_NE(what.find("t.env:1"), std::string::npos) << what;
            EXPECT_NE(what.find("is not set"), std::string::npos) << what;
        }
    }
}

// ------------------------------------------------------------------ boot gate

TEST(BootGate, HardFailuresStopStrictEnvironmentsOnly)
{
    snicholls::boot_gate gate;
    gate.add("ok", check_severity::hard, [] { return check_result::pass("fine"); });
    gate.add("disk", check_severity::soft, [] { return check_result::fail("80% full"); });
    EXPECT_FALSE(gate.run().hard_failed());
    EXPECT_TRUE(gate.enforce(run_environment::production).is_ok());   // a soft failure never stops a start

    gate.add("database", check_severity::hard, []() -> check_result { throw std::runtime_error("refused"); });
    const auto report = gate.run();
    ASSERT_EQ(report.items.size(), 3u);
    EXPECT_TRUE(report.hard_failed());
    EXPECT_FALSE(report.items[2].passed);
    EXPECT_NE(report.items[2].detail.find("check threw: refused"), std::string::npos);
    EXPECT_NE(report.failures().find("database (hard)"), std::string::npos) << report.failures();
    EXPECT_NE(report.to_json().find("\"passed\":false"), std::string::npos);

    EXPECT_TRUE(gate.enforce(run_environment::local).is_ok());          // reported, not enforced
    EXPECT_TRUE(gate.enforce(run_environment::staging).is_err());
    const auto prod = gate.enforce(run_environment::production);
    ASSERT_TRUE(prod.is_err());
    EXPECT_TRUE(prod.unwrap_err_unchecked().hard_failed());
}

TEST(BootGate, PublishesEachCheckAsAHealthComponent)
{
    snicholls::boot_gate gate;
    gate.add("clock", check_severity::hard, [] { return check_result::pass(); });
    gate.add("disk", check_severity::soft, [] { return check_result::fail("full"); });
    gate.add("db", check_severity::hard, [] { return check_result::fail("down"); });
    snicholls::health_registry health;
    snicholls::boot_gate::publish(health, gate.run());
    const auto r = health.report();
    ASSERT_EQ(r.components.size(), 3u);
    EXPECT_EQ(r.overall, snicholls::health_state::unhealthy);
    for (const auto& c : r.components) {
        if (c.name == "boot.clock") { EXPECT_EQ(c.state, snicholls::health_state::ready); }
        if (c.name == "boot.disk") { EXPECT_EQ(c.state, snicholls::health_state::degraded); }
        if (c.name == "boot.db") { EXPECT_EQ(c.state, snicholls::health_state::unhealthy); }
    }
}

TEST(BootGate, ClockCheckRejectsAnUnsetClockAndPassesASaneOne)
{
    using namespace std::chrono;
    const auto sane = snicholls::clock_check();
    EXPECT_TRUE(sane().passed);   // baseline
    EXPECT_TRUE(sane().passed);   // no step since
    const auto future_floor = snicholls::clock_check(sys_days{year{2400} / January / 1});
    const auto r = future_floor();
    EXPECT_FALSE(r.passed);
    EXPECT_NE(r.detail.find("2400"), std::string::npos) << r.detail;
}

// ------------------------------------------------------------------ boot_monitor

TEST(BootMonitor, ReportsRegressionsAndRecoveriesAndStopsWhenDestroyed)
{
    snicholls::event_loop loop;
    std::atomic<bool> db_up{true};
    auto gate = std::make_shared<snicholls::boot_gate>();
    gate->add("db", check_severity::hard,
              [&] { return db_up.load() ? check_result::pass() : check_result::fail("connection refused"); });
    auto health = std::make_shared<snicholls::health_registry>();
    std::vector<std::string> changes;
    auto runs = std::make_shared<std::atomic<int>>(0);
    gate->add("counter", check_severity::soft, [runs] {
        ++*runs;
        return check_result::pass();
    });
    {
        snicholls::boot_monitor monitor(
            loop, gate, 5ms,
            [&](const snicholls::boot_change& c) { changes.push_back(c.name + (c.passed ? " recovered" : " failed")); },
            health);
        monitor.check_now();
        EXPECT_TRUE(changes.empty());   // all passing on the first run: nothing to report
        db_up = false;
        const auto until = std::chrono::steady_clock::now() + 5s;
        while (changes.empty() && std::chrono::steady_clock::now() < until) loop.run_once(10ms);
        EXPECT_EQ(changes, (std::vector<std::string>{"db failed"}));
        EXPECT_EQ(health->report().overall, snicholls::health_state::unhealthy);
        db_up = true;
        while (changes.size() < 2 && std::chrono::steady_clock::now() < until) loop.run_once(10ms);
        EXPECT_EQ(changes, (std::vector<std::string>{"db failed", "db recovered"}));
        EXPECT_EQ(health->report().overall, snicholls::health_state::ready);
        EXPECT_TRUE(monitor.last().all_passed());
    }
    const int after = runs->load();
    for (int i = 0; i < 5; ++i) loop.run_once(10ms);
    EXPECT_EQ(runs->load(), after);   // destroyed: no more runs
}

// ------------------------------------------------------------------ signals on an event loop

TEST(Signals, DeliveredAsTasksOnTheEventLoop)
{
    snicholls::event_loop loop;
    std::vector<int> seen;
    std::thread::id where;
    {
        snicholls::signal_watcher watcher(loop, [&](int sig) {
            seen.push_back(sig);
            where = std::this_thread::get_id();
        });
        ::raise(SIGUSR1);
        ::raise(SIGHUP);
        const auto until = std::chrono::steady_clock::now() + 5s;
        while (seen.size() < 2 && std::chrono::steady_clock::now() < until) loop.run_once(10ms);
        EXPECT_EQ(watcher.handled(), 2);
    }
    EXPECT_EQ(seen, (std::vector<int>{SIGUSR1, SIGHUP}));
    EXPECT_EQ(where, std::this_thread::get_id());   // the loop's thread, which is this one
    // The loop form and the thread form share the one-watcher rule
    snicholls::signal_watcher first(loop, [](int) {});
    EXPECT_THROW(snicholls::signal_watcher([](int) {}), std::logic_error);
}

TEST(Signals, ScopedIgnoreSigpipeRestoresThePreviousDisposition)
{
    struct sigaction before {};
    ::sigaction(SIGPIPE, nullptr, &before);
    {
        snicholls::scoped_ignore_sigpipe ignore;
        int fds[2];
        ASSERT_EQ(::pipe(fds), 0);
        ::close(fds[0]);
        const char byte = 'x';
        EXPECT_EQ(::write(fds[1], &byte, 1), -1);
        EXPECT_EQ(errno, EPIPE);
        ::close(fds[1]);
    }
    struct sigaction after {};
    ::sigaction(SIGPIPE, nullptr, &after);
    EXPECT_TRUE(after.sa_handler == before.sa_handler);
}

// ------------------------------------------------------------------ /healthz and /readyz

int get_status(std::uint16_t port, const char* path)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        ::close(fd);
        return -1;
    }
    const std::string req = std::string("GET ") + path + " HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n";
    [[maybe_unused]] auto sent = ::send(fd, req.data(), req.size(), 0);
    std::string got;
    char buf[2048];
    ssize_t n;
    while ((n = ::recv(fd, buf, sizeof buf, 0)) > 0) got.append(buf, static_cast<std::size_t>(n));
    ::close(fd);
    if (got.size() < 12 || got.find("no-store") == std::string::npos) return -2;
    return std::stoi(got.substr(9, 3));
}

TEST(HttpHealth, LiveAndReadyAnswer200Or503)
{
    auto health = std::make_shared<snicholls::health_registry>();
    snicholls::http::server srv;
    snicholls::serve_health(srv, health);
    const std::uint16_t port = srv.listen("127.0.0.1", 0);
    std::thread t([&] { srv.run(); });
    while (!srv.running()) std::this_thread::yield();

    health->set("db", snicholls::health_state::starting);
    EXPECT_EQ(get_status(port, "/healthz"), 200);   // live while starting
    EXPECT_EQ(get_status(port, "/readyz"), 503);    // but not ready
    health->set("db", snicholls::health_state::degraded);
    EXPECT_EQ(get_status(port, "/readyz"), 200);
    health->set("db", snicholls::health_state::unhealthy);
    EXPECT_EQ(get_status(port, "/healthz"), 503);
    health->set("db", snicholls::health_state::ready);
    health->set_stopping(true);
    EXPECT_EQ(get_status(port, "/readyz"), 503);

    srv.stop();
    t.join();
}

} // namespace
