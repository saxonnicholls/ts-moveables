//
//  tests_global.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - tests for process/global.hpp: constructed once under contention, dependencies shut
//  down in reverse, a throwing constructor is retried, reinitialisation only when allowed, a one-thread cycle throws,
//  global_scope, and the observer. Ported from an earlier application framework's global_init tests.
//

#include "test_helpers.hpp"

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "../TSMoveables/process/global.hpp"

namespace {

using snicholls::global;
using snicholls::global_init_error;

std::vector<std::string>& events()
{
    static std::vector<std::string> e;
    return e;
}

struct counted
{
    static constexpr std::string_view name = "counted";
    static constexpr bool reinitialisable = true;
    static inline std::atomic<int> constructions{0};
    counted()
    {
        ++constructions;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));   // widen the race window
    }
};

struct base_sdk
{
    static constexpr std::string_view name = "base_sdk";
    static constexpr bool reinitialisable = true;
    base_sdk() { events().push_back("init base"); }
    ~base_sdk() { events().push_back("stop base"); }
};

struct dependent_sdk
{
    static constexpr std::string_view name = "dependent_sdk";
    static constexpr bool reinitialisable = true;
    dependent_sdk()
    {
        global<base_sdk>::get();   // the dependency completes first, so it shuts down last
        events().push_back("init dependent");
    }
    ~dependent_sdk() { events().push_back("stop dependent"); }
};

struct flaky
{
    static constexpr std::string_view name = "flaky";
    static constexpr bool reinitialisable = true;
    static inline int attempts = 0;
    flaky()
    {
        if (++attempts == 1) throw std::runtime_error("network down");
    }
};

struct once_only
{
    static constexpr std::string_view name = "once_only";   // not reinitialisable, like the AWS SDK
};

struct cyclic_b;
struct cyclic_a
{
    static constexpr std::string_view name = "cyclic_a";
    cyclic_a();
};
struct cyclic_b
{
    static constexpr std::string_view name = "cyclic_b";
    cyclic_b() { global<cyclic_a>::get(); }
};
cyclic_a::cyclic_a() { global<cyclic_b>::get(); }

struct observed
{
    static constexpr std::string_view name = "observed";
    static constexpr bool reinitialisable = true;
};

template <class F>
bool throws_init_error(F f)
{
    try {
        f();
    } catch (const global_init_error&) {
        return true;
    }
    return false;
}

void test_constructed_once_under_contention()
{
    counted::constructions = 0;
    std::vector<std::thread> threads;
    std::atomic<counted*> first{nullptr};
    std::atomic<int> mismatches{0};
    for (int i = 0; i < 16; ++i)
        threads.emplace_back([&] {
            counted* p = &global<counted>::get();
            counted* expected = nullptr;
            if (!first.compare_exchange_strong(expected, p) && expected != p) ++mismatches;
        });
    for (auto& t : threads) t.join();
    assert(counted::constructions.load() == 1);
    assert(mismatches.load() == 0);
    assert(global<counted>::initialised());
    snicholls::shutdown_globals();
    pass("global: 16 racing first callers construct it exactly once and all get the same instance");
}

void test_dependencies_shut_down_in_reverse()
{
    events().clear();
    global<dependent_sdk>::get();
    assert(snicholls::active_globals() == (std::vector<std::string>{"base_sdk", "dependent_sdk"}));
    const auto order = snicholls::shutdown_globals();
    assert(order == (std::vector<std::string>{"dependent_sdk", "base_sdk"}));
    assert(events() == (std::vector<std::string>{"init base", "init dependent", "stop dependent", "stop base"}));
    assert(!global<base_sdk>::initialised());
    assert(global<base_sdk>::shut_down());
    assert(snicholls::active_globals().empty());
    pass("global: a dependency completes first and shuts down last");
}

void test_throwing_constructor_is_retried()
{
    flaky::attempts = 0;
    bool threw = false;
    try {
        global<flaky>::get();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw && !global<flaky>::initialised());
    global<flaky>::get();
    assert(flaky::attempts == 2 && global<flaky>::initialised());
    snicholls::shutdown_globals();
    pass("global: a constructor that throws leaves it uninitialised, and the next get() retries");
}

void test_reinitialisation_only_when_allowed()
{
    global<once_only>::get();
    snicholls::shutdown_globals();
    assert(throws_init_error([] { global<once_only>::get(); }));

    events().clear();
    global<base_sdk>::get();   // reinitialisable
    snicholls::shutdown_globals();
    global<base_sdk>::get();
    assert(!global<base_sdk>::shut_down());
    snicholls::shutdown_globals();
    pass("global: after shutdown, get() restarts only a reinitialisable global");
}

void test_cycle_on_one_thread_throws()
{
    assert(throws_init_error([] { global<cyclic_a>::get(); }));
    assert(!global<cyclic_a>::initialised());
    assert(!global<cyclic_b>::initialised());
    pass("global: a cycle on one thread throws global_init_error instead of deadlocking");
}

void test_global_scope_shuts_down()
{
    events().clear();
    {
        const snicholls::global_scope scope;
        global<base_sdk>::get();
    }
    assert(events().back() == "stop base");
    assert(snicholls::shutdown_globals().empty());
    pass("global: a global_scope shuts everything down when it ends");
}

std::vector<std::string>& seen()
{
    static std::vector<std::string> s;
    return s;
}

void record(snicholls::global_event e, std::string_view name) noexcept
{
    using snicholls::global_event;
    const char* what = e == global_event::initialising ? "initialising " :
                       e == global_event::initialised  ? "initialised " : "shutting down ";
    seen().push_back(what + std::string(name));
}

void test_observer_sees_every_start_and_stop()
{
    seen().clear();
    const auto previous = snicholls::set_global_observer(&record);
    global<observed>::get();
    global<observed>::get();   // already initialised: no event
    snicholls::shutdown_globals();
    assert(snicholls::set_global_observer(previous) == &record);
    assert(seen() == (std::vector<std::string>{"initialising observed", "initialised observed",
                                               "shutting down observed"}));
    pass("global: the observer is told of each start and stop, once");
}

} // namespace

void run_global_tests()
{
    test_constructed_once_under_contention();
    test_dependencies_shut_down_in_reverse();
    test_throwing_constructor_is_retried();
    test_reinitialisation_only_when_allowed();
    test_cycle_on_one_thread_throws();
    test_global_scope_shuts_down();
    test_observer_sees_every_start_and_stop();
}
