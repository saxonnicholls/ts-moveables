//
//  health.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - is this process healthy, is it ready, and may it
//  start serving at all:
//
//      auto health = std::make_shared<snicholls::health_registry>();
//      health->set("database", snicholls::health_state::ready);
//      health->add_probe("queue", [&] { return std::vector<snicholls::component_health>{...}; });
//      auto report = health->report();               // live(), ready(), to_json()
//
//      auto gate = std::make_shared<snicholls::boot_gate>();
//      gate->add("clock", snicholls::check_severity::hard, snicholls::clock_check());
//      gate->add("database", snicholls::check_severity::hard, [&] { return ping(); });
//      if (auto r = gate->enforce(env); r.is_err()) {             // hard failure in staging/production
//          std::cerr << r.unwrap_err_unchecked().failures();
//          return snicholls::to_int(snicholls::exit_code::unavailable);
//      }
//      snicholls::boot_monitor monitor(loop, gate, std::chrono::seconds{30}, on_change, health);
//
//  The registry (ported): components set by name, and probes run at
//  report time outside its lock (a probe that throws is an unhealthy
//  component, not a crash). The worst component decides: ready < degraded <
//  starting < unhealthy < stopping. live() is "do not restart me" (anything
//  but unhealthy); ready() is "send me traffic" (ready or degraded).
//
//  service_notifier (ported) speaks systemd's notify protocol - READY,
//  RELOADING, STOPPING, WATCHDOG, STATUS - over $NOTIFY_SOCKET with no
//  libsystemd. health_file writes the report atomically for an exec probe
//  (Docker HEALTHCHECK, a Kubernetes exec probe) to read.
//
//  New here, after an earlier service's start-up checks: the boot gate (hard and
//  soft checks; enforce() returns an error in staging and production and the
//  caller decides what to do - library code never exits the process), a
//  clock check, and boot_monitor, which re-runs the gate on an event-loop
//  timer, reports regressions and recoveries, and stops when destroyed (the
//  copy it replaces ran a detached thread nothing could stop). Checks for
//  tzdata and NTP wait for core's time_zone interface. /healthz and /readyz
//  for http::server are in http_health.hpp.
//
//  C++20 and POSIX, opt-in like fs/.
//

#ifndef ts_moveables_process_health_hpp
#define ts_moveables_process_health_hpp

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <atomic>
#include <memory>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <snicholls/core/result.hpp>

#include "../event/loop.hpp"
#include "../fs/file.hpp"
#include "../utils/json.hpp"
#include "environment.hpp"

namespace snicholls {

enum class health_state
{
    ready,   ///< ready
    degraded,   ///< working with reduced capacity (e.g. a worker restarting)
    starting,   ///< starting
    unhealthy,   ///< needs a restart
    stopping,   ///< stopping
};

[[nodiscard]] constexpr std::string_view to_string(health_state s) noexcept
{
    switch (s)
    {
    case health_state::ready: return "ready";
    case health_state::degraded: return "degraded";
    case health_state::starting: return "starting";
    case health_state::unhealthy: return "unhealthy";
    case health_state::stopping: return "stopping";
    }
    return "?";
}

namespace detail::health_detail {

inline void append_json_string(std::string& out, std::string_view s)
{
    out += '"';
    utils::json_escape(s, out);
    out += '"';
}

constexpr int health_rank(health_state s) noexcept
{
    switch (s)
    {
    case health_state::ready: return 0;
    case health_state::degraded: return 1;
    case health_state::starting: return 2;
    case health_state::unhealthy: return 3;
    case health_state::stopping: return 4;
    }
    return 3;
}

} // namespace detail::health_detail

struct component_health
{
    std::string name;
    health_state state = health_state::starting;
    std::string detail;
};

struct health_report
{
    health_state overall = health_state::ready;
    std::vector<component_health> components;

    [[nodiscard]] bool live() const noexcept { return overall != health_state::unhealthy; }
    [[nodiscard]] bool ready() const noexcept
    {
        return overall == health_state::ready || overall == health_state::degraded;
    }

    [[nodiscard]] std::string to_json() const
    {
        std::string out = "{\"status\":";
        detail::health_detail::append_json_string(out, to_string(overall));
        out += ",\"live\":";
        out += live() ? "true" : "false";
        out += ",\"ready\":";
        out += ready() ? "true" : "false";
        out += ",\"components\":[";
        for (std::size_t i = 0; i < components.size(); ++i)
        {
            if (i != 0)
                out += ',';
            out += "{\"name\":";
            detail::health_detail::append_json_string(out, components[i].name);
            out += ",\"status\":";
            detail::health_detail::append_json_string(out, to_string(components[i].state));
            out += ",\"detail\":";
            detail::health_detail::append_json_string(out, components[i].detail);
            out += '}';
        }
        out += "]}";
        return out;
    }
};

[[nodiscard]] inline health_state aggregate(const std::vector<component_health>& components) noexcept
{
    health_state worst = health_state::ready;
    for (const component_health& c : components)
        if (detail::health_detail::health_rank(c.state) > detail::health_detail::health_rank(worst))
            worst = c.state;
    return worst;
}

class health_registry
{
public:
    using probe = std::function<std::vector<component_health>()>;

    void set(std::string name, health_state state, std::string detail = {})
    {
        const std::lock_guard lock(mutex_);
        components_[std::move(name)] = {state, std::move(detail)};
    }

    void remove(std::string_view name)
    {
        const std::lock_guard lock(mutex_);
        if (const auto it = components_.find(name); it != components_.end())
            components_.erase(it);
    }

    void add_probe(std::string name, probe p)
    {
        const std::lock_guard lock(mutex_);
        probes_[std::move(name)] = std::move(p);
    }

    void remove_probe(std::string_view name)
    {
        const std::lock_guard lock(mutex_);
        if (const auto it = probes_.find(name); it != probes_.end())
            probes_.erase(it);
    }

    void set_stopping(bool stopping)
    {
        const std::lock_guard lock(mutex_);
        stopping_ = stopping;
    }

    [[nodiscard]] health_report report() const
    {
        health_report r;
        std::vector<std::pair<std::string, probe>> probes;
        bool stopping;
        {
            const std::lock_guard lock(mutex_);
            for (const auto& [name, c] : components_)
                r.components.push_back({name, c.first, c.second});
            probes.assign(probes_.begin(), probes_.end());
            stopping = stopping_;
        }
        for (const auto& [name, p] : probes)
        {
            try
            {
                for (component_health& c : p())
                    r.components.push_back(std::move(c));
            }
            catch (const std::exception& e)
            {
                r.components.push_back({name, health_state::unhealthy, std::string("probe threw: ") + e.what()});
            }
            catch (...)
            {
                r.components.push_back({name, health_state::unhealthy, "probe threw"});
            }
        }
        r.overall = stopping ? health_state::stopping : aggregate(r.components);
        return r;
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::pair<health_state, std::string>, std::less<>> components_;
    std::map<std::string, probe, std::less<>> probes_;
    bool stopping_ = false;
};

// ---------------------------------------------------------------------------------------------------------------
// systemd notification
// ---------------------------------------------------------------------------------------------------------------

class service_notifier
{
public:
    service_notifier() = default;   // disabled
    explicit service_notifier(std::string socket_path, std::chrono::microseconds watchdog = {})
        : socket_path_(std::move(socket_path)), watchdog_(watchdog)
    {
    }

    [[nodiscard]] static service_notifier from_environment(const std::map<std::string, std::string>& env)
    {
        const auto get = [&](const char* k) -> std::optional<std::string> {
            const auto it = env.find(k);
            return it == env.end() ? std::nullopt : std::optional<std::string>(it->second);
        };
        std::chrono::microseconds watchdog{};
        if (const auto usec = get("WATCHDOG_USEC"))
        {
            const auto pid = get("WATCHDOG_PID");
            if (!pid || *pid == std::to_string(::getpid()))
                watchdog = std::chrono::microseconds(std::strtoll(usec->c_str(), nullptr, 10));
        }
        return service_notifier(get("NOTIFY_SOCKET").value_or(std::string()), watchdog);
    }

    [[nodiscard]] bool enabled() const noexcept { return !socket_path_.empty(); }

    [[nodiscard]] std::chrono::microseconds watchdog_interval() const noexcept { return watchdog_; }

    bool notify(std::string_view state) const noexcept
    {
        if (!enabled() || socket_path_.size() >= sizeof(::sockaddr_un{}.sun_path))
            return false;
        const int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
        if (fd < 0)
            return false;
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        ::sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, socket_path_.data(), socket_path_.size());
        if (address.sun_path[0] == '@')
            address.sun_path[0] = '\0';   // Linux abstract namespace
        const auto length = static_cast<::socklen_t>(offsetof(::sockaddr_un, sun_path) + socket_path_.size());
        const ::ssize_t sent =
            ::sendto(fd, state.data(), state.size(), 0, reinterpret_cast<const ::sockaddr*>(&address), length);
        ::close(fd);
        return sent == static_cast<::ssize_t>(state.size());
    }

    bool ready() const noexcept { return notify("READY=1"); }
    bool stopping() const noexcept { return notify("STOPPING=1"); }
    bool watchdog() const noexcept { return notify("WATCHDOG=1"); }

    bool reloading() const
    {
        ::timespec ts{};
        ::clock_gettime(CLOCK_MONOTONIC, &ts);
        const long long usec = static_cast<long long>(ts.tv_sec) * 1'000'000LL + ts.tv_nsec / 1000;
        return notify("RELOADING=1\nMONOTONIC_USEC=" + std::to_string(usec));
    }

    bool status(std::string_view text) const { return notify("STATUS=" + std::string(text)); }

private:
    std::string socket_path_;
    std::chrono::microseconds watchdog_{};
};

// ---------------------------------------------------------------------------------------------------------------
// Health file
// ---------------------------------------------------------------------------------------------------------------

class health_file
{
public:
    explicit health_file(std::filesystem::path path) : path_(std::move(path)) {}

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    // Replace the file with the report, atomically: a probe never reads half a report
    bool write(const health_report& report) const noexcept
    {
        try
        {
            return write_file_atomically(path_, report.to_json() + "\n", {.durable = false}).is_ok();
        }
        catch (...)
        {
            return false;
        }
    }

    void remove() const noexcept
    {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

private:
    std::filesystem::path path_;
};

// ---------------------------------------------------------------------------------------------------------------
// Boot gates
// ---------------------------------------------------------------------------------------------------------------

// A hard check must pass before a strict (staging, production) process serves; a soft one is reported only
enum class check_severity
{
    hard,
    soft,
};

[[nodiscard]] constexpr std::string_view to_string(check_severity s) noexcept
{
    return s == check_severity::hard ? "hard" : "soft";
}

struct check_result
{
    bool passed = true;
    std::string detail;

    [[nodiscard]] static check_result pass(std::string detail = {}) { return {true, std::move(detail)}; }
    [[nodiscard]] static check_result fail(std::string detail) { return {false, std::move(detail)}; }
};

using boot_check = std::function<check_result()>;

struct boot_report
{
    struct item
    {
        std::string name;
        check_severity severity = check_severity::hard;
        bool passed = true;
        std::string detail;
    };
    std::vector<item> items;

    [[nodiscard]] bool hard_failed() const noexcept
    {
        return std::any_of(items.begin(), items.end(),
                           [](const item& i) { return !i.passed && i.severity == check_severity::hard; });
    }
    [[nodiscard]] bool all_passed() const noexcept
    {
        return std::all_of(items.begin(), items.end(), [](const item& i) { return i.passed; });
    }

    // One line per failure, for a log or an exception message
    [[nodiscard]] std::string failures() const
    {
        std::string out;
        for (const item& i : items)
            if (!i.passed)
            {
                if (!out.empty())
                    out += "; ";
                out += i.name + " (" + std::string(to_string(i.severity)) + "): " + i.detail;
            }
        return out;
    }

    [[nodiscard]] std::string to_json() const
    {
        std::string out = "{\"passed\":";
        out += all_passed() ? "true" : "false";
        out += ",\"checks\":[";
        for (std::size_t n = 0; n < items.size(); ++n)
        {
            if (n != 0)
                out += ',';
            out += "{\"name\":";
            detail::health_detail::append_json_string(out, items[n].name);
            out += ",\"severity\":";
            detail::health_detail::append_json_string(out, to_string(items[n].severity));
            out += ",\"passed\":";
            out += items[n].passed ? "true" : "false";
            out += ",\"detail\":";
            detail::health_detail::append_json_string(out, items[n].detail);
            out += '}';
        }
        out += "]}";
        return out;
    }
};

// Checks that must pass before a process serves: register them, run them all, decide. Never exits the process:
// enforce() returns the report as an error and the caller decides (usually exit_code::unavailable)
class boot_gate
{
public:
    void add(std::string name, check_severity severity, boot_check check)
    {
        const std::lock_guard lock(mutex_);
        checks_.push_back({std::move(name), severity, std::move(check)});
    }

    // Every check, in the order added; one that throws has failed
    [[nodiscard]] boot_report run() const
    {
        std::vector<entry> checks;
        {
            const std::lock_guard lock(mutex_);
            checks = checks_;
        }
        boot_report report;
        for (const entry& e : checks)
        {
            check_result r;
            try
            {
                r = e.check();
            }
            catch (const std::exception& ex)
            {
                r = check_result::fail(std::string("check threw: ") + ex.what());
            }
            catch (...)
            {
                r = check_result::fail("check threw");
            }
            report.items.push_back({e.name, e.severity, r.passed, std::move(r.detail)});
        }
        return report;
    }

    // The report, or the report as an error when a hard check failed and `env` is strict (staging, production).
    // Locally a hard failure is reported but does not stop the process
    [[nodiscard]] result<boot_report, boot_report> enforce(run_environment env) const
    {
        boot_report report = run();
        if (report.hard_failed() && is_strict(env))
            return err(std::move(report));
        return ok(std::move(report));
    }

    // Each check as a health component: passed is ready, a failed soft check degraded, a failed hard one unhealthy
    static void publish(health_registry& health, const boot_report& report, std::string_view prefix = "boot.")
    {
        for (const boot_report::item& i : report.items)
            health.set(std::string(prefix) + i.name,
                       i.passed ? health_state::ready
                                : (i.severity == check_severity::hard ? health_state::unhealthy : health_state::degraded),
                       i.detail);
    }

private:
    struct entry
    {
        std::string name;
        check_severity severity;
        boot_check check;
    };
    mutable std::mutex mutex_;
    std::vector<entry> checks_;
};

// The system clock is believable: not before `floor` (a machine that booted with no RTC battery and no NTP says
// 1970), and not stepped by more than `max_step` against the monotonic clock since the previous run of this check
// (an NTP correction, a VM resumed from suspend, someone running `date -s`). The first run sets the baseline
[[nodiscard]] inline boot_check clock_check(std::chrono::sys_days floor = std::chrono::sys_days{std::chrono::year{2026} /
                                                                                                std::chrono::January / 1},
                                            std::chrono::seconds max_step = std::chrono::seconds{2})
{
    struct baseline
    {
        std::mutex m;
        bool set = false;
        std::chrono::nanoseconds offset{};   // system - steady
    };
    auto state = std::make_shared<baseline>();
    return [floor, max_step, state]() -> check_result {
        const auto system = std::chrono::system_clock::now();
        const auto steady = std::chrono::steady_clock::now();
        // At seconds: system_clock may count nanoseconds, and a floor past 2262 would overflow them
        if (std::chrono::floor<std::chrono::seconds>(system) < std::chrono::sys_seconds(floor))
            return check_result::fail("the system clock is before " + std::to_string(int(std::chrono::year_month_day(floor).year())) +
                                      ": it has not been set");
        const auto offset = std::chrono::duration_cast<std::chrono::nanoseconds>(system.time_since_epoch()) -
                            std::chrono::duration_cast<std::chrono::nanoseconds>(steady.time_since_epoch());
        const std::lock_guard lock(state->m);
        if (!state->set)
        {
            state->set = true;
            state->offset = offset;
            return check_result::pass("baseline taken");
        }
        const auto step = offset - state->offset;
        state->offset = offset;
        const auto magnitude = step < std::chrono::nanoseconds::zero() ? -step : step;
        if (magnitude > max_step)
            return check_result::fail("the system clock stepped by " +
                                      std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(step).count()) +
                                      " ms against the monotonic clock");
        return check_result::pass();
    };
}

#if SNICHOLLS_HAS_EVENT_LOOP

// A check that changed: it failed after passing (a regression) or passed after failing (a recovery)
struct boot_change
{
    std::string name;
    check_severity severity = check_severity::hard;
    bool passed = true;
    std::string detail;
};

// Re-runs a gate every `interval` on an event loop, publishes each run to a health registry if one is given, and
// reports each regression and recovery. Construct and destroy it on the loop's thread (or before run()); destroying
// it stops the re-checks. The checks run on the loop thread, so keep them quick
class boot_monitor
{
public:
    using on_change_fn = std::function<void(const boot_change&)>;

    boot_monitor(event_loop& loop, std::shared_ptr<const boot_gate> gate, std::chrono::milliseconds interval,
                 on_change_fn on_change = {}, std::shared_ptr<health_registry> health = {})
        : state_(std::make_shared<state>())
    {
        state_->gate = std::move(gate);
        state_->on_change = std::move(on_change);
        state_->health = std::move(health);
        timer_ = loop.every(interval);
        std::weak_ptr<state> weak = state_;
        timer_.on_fire([weak] {
            if (auto s = weak.lock())
                s->tick();
        });
    }

    boot_monitor(const boot_monitor&) = delete;
    boot_monitor& operator=(const boot_monitor&) = delete;

    // Runs a check now, as the timer would
    void check_now() { state_->tick(); }

    // The most recent report
    [[nodiscard]] boot_report last() const
    {
        const std::lock_guard lock(state_->m);
        return state_->last;
    }

private:
    struct state
    {
        std::shared_ptr<const boot_gate> gate;
        on_change_fn on_change;
        std::shared_ptr<health_registry> health;
        mutable std::mutex m;
        boot_report last;
        bool have_last = false;

        void tick()
        {
            boot_report now = gate->run();
            if (health)
                boot_gate::publish(*health, now);
            std::vector<boot_change> changes;
            {
                const std::lock_guard lock(m);
                for (const boot_report::item& i : now.items)
                {
                    bool was_passed = true;   // first run: only failures are news
                    if (have_last)
                        for (const boot_report::item& p : last.items)
                            if (p.name == i.name)
                                was_passed = p.passed;
                    if (i.passed != was_passed)
                        changes.push_back({i.name, i.severity, i.passed, i.detail});
                }
                last = now;
                have_last = true;
            }
            if (on_change)
                for (const boot_change& c : changes)
                    on_change(c);
        }
    };

    std::shared_ptr<state> state_;
    event_loop::timer timer_;
};

#endif // SNICHOLLS_HAS_EVENT_LOOP

} // namespace snicholls

#endif // ts_moveables_process_health_hpp
