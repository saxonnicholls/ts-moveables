//
//  tests_logging.cpp
//  TSMoveables
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - unit tests for logging and telemetry
//
//  The claims worth testing are the ones a logger is usually vague about:
//  that producers never block, that a slow sink cannot hold up a fast one,
//  that overflow is counted rather than silently swallowed, and that nothing
//  queued at shutdown is lost.
//

#include "test_helpers.hpp"

#include "../TSMoveables/logging/logger.hpp"
#include "../TSMoveables/moveable/mutex.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace snicholls;
using namespace snicholls::log;
using namespace std::chrono_literals;

namespace {

void test_log_basics()
{
    logger_config cfg;
    cfg.name = "unit";
    logger lg{cfg};

    std::vector<record> seen;
    moveable_mutex<> m;
    lg.add_sink([&](const record& r) {
        std::lock_guard<moveable_mutex<>> g(m);
        seen.push_back(r);
    });

    SN_LOGGER_INFO(lg) << "hello " << 42 << " world";
    SN_LOGGER_WARN(lg).field("venue", "XLON").field("qty", 100) << "reject";
    lg.flush();

    assert(seen.size() == 2);
    assert(seen[0].level == level::info);
    assert(seen[0].message == "hello 42 world");
    assert(seen[0].logger == "unit");
    assert(seen[0].line > 0 && seen[0].file[0] != '\0');   // call site captured
    assert(seen[1].level == level::warn);
    assert(seen[1].fields.size() == 2);
    assert(seen[1].fields[0].first == "venue" && seen[1].fields[0].second == "XLON");

    pass("logging: levels, stream syntax, structured fields, call site");
}

void test_log_level_filtering()
{
    logger lg;
    std::atomic<int> n{0};
    lg.add_sink([&](const record&) { n.fetch_add(1); });

    lg.set_level(level::warn);
    // A disabled statement must not even evaluate its arguments - that is the
    // whole point of testing the level before building the record
    std::atomic<int> evaluated{0};
    auto costly = [&] { evaluated.fetch_add(1); return 7; };
    SN_LOGGER_INFO(lg) << costly();
    SN_LOGGER_DEBUG(lg) << costly();
    SN_LOGGER_ERROR(lg) << "kept";
    lg.flush();

    assert(n.load() == 1);
    assert(evaluated.load() == 0);      // the arguments were never built

    lg.set_level(level::off);
    SN_LOGGER_CRITICAL(lg) << "silenced";
    lg.flush();
    assert(n.load() == 1);

    pass("logging: level filter skips argument evaluation entirely");
}

void test_log_many_threads()
{
    logger_config cfg;
    cfg.default_lane.capacity = 1 << 16;
    logger lg{cfg};

    std::atomic<long long> got{0};
    lg.add_sink([&](const record&) { got.fetch_add(1, std::memory_order_relaxed); });

    constexpr int threads = 8;
    // static, because the producer lambda below captures explicitly and has no
    // default capture mode - see the note in test_helpers.hpp
    static constexpr int per_thread = 2000;
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t)
        ts.emplace_back([&lg] {
            for (int i = 0; i < per_thread; ++i)
                SN_LOGGER_INFO(lg) << "from thread " << i;
        });
    for (auto& t : ts)
        t.join();
    lg.flush();

    assert(lg.dropped() == 0);
    assert(got.load() == threads * per_thread);

    pass("logging: 8 threads, 16000 records, none lost");
}

void test_log_lanes_are_independent()
{
    // The reason lanes exist: a slow sink must not hold up a fast one.
    //
    // Both counters are declared BEFORE the logger so that they are destroyed
    // AFTER it. This test is the one place that deliberately leaves records in
    // flight - it asserts the slow lane dropped some - so the slow lane's
    // drain thread is still running at scope exit, and it is the logger's
    // destructor that joins it. Declared the other way round, the counters
    // would be destroyed first and that thread would touch dead stack. Which
    // is exactly what happened: ASan caught it here, nowhere else, because
    // every other test in this file flushes first. The rule this encodes is
    // the one users need too - whatever a sink captures by reference must
    // outlive the logger.
    std::atomic<int> fast{0};
    std::atomic<int> slow{0};

    logger lg;
    lg.add_sink([&](const record&) { fast.fetch_add(1, std::memory_order_relaxed); });

    auto slow_lane = lg.add_lane("slow", 8, overflow::drop_newest);
    slow_lane.add_sink([&](const record&) {
        std::this_thread::sleep_for(2ms);           // a database, a network hop
        slow.fetch_add(1, std::memory_order_relaxed);
    });

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; ++i)
        SN_LOGGER_INFO(lg) << "record " << i;
    const double produce_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    // Producing never waited on the slow lane: 200 records against a sink that
    // takes 2 ms each would be 400 ms if it did
    assert(produce_ms < 200.0);

    // The fast lane completes regardless of what the slow one is doing
    spin_until([&] { return fast.load() == 200; });
    assert(fast.load() == 200);
    assert(lg.lane_count() == 2);

    // The slow lane's bounded queue dropped rather than blocking anyone, and
    // said so
    assert(slow_lane.dropped() > 0);

    pass("logging: a slow lane cannot stall a fast one, and counts its drops");
}

void test_log_overflow_counted()
{
    logger_config cfg;
    cfg.default_lane.capacity = 8;
    logger lg{cfg};

    std::atomic<int> delivered{0};
    lg.add_sink([&](const record&) {
        std::this_thread::sleep_for(500us);
        delivered.fetch_add(1);
    });

    for (int i = 0; i < 500; ++i)
        SN_LOGGER_INFO(lg) << i;
    lg.flush();

    // Whatever the split, nothing vanishes unaccounted for
    assert(lg.dropped() > 0);
    assert(delivered.load() > 0);
    assert(std::uint64_t(delivered.load()) + lg.dropped() >= 500);

    pass("logging: overflow drops are counted, never silent");
}

void test_log_json_and_metrics()
{
    logger lg;
    std::vector<std::string> lines;
    moveable_mutex<> m;
    lg.add_sink(json_sink([&](const std::string& j) {
        std::lock_guard<moveable_mutex<>> g(m);
        lines.push_back(j);
    }));

    SN_LOGGER_ERROR(lg).field("order", 77) << "bad \"quote\" and \\ backslash";
    lg.metric("orders.filled", 12.5, {{"venue", "XLON"}});
    lg.flush();

    assert(lines.size() == 2);
    // Escaping is the thing that quietly corrupts a log pipeline
    assert(lines[0].find("\\\"quote\\\"") != std::string::npos);
    assert(lines[0].find("\\\\ backslash") != std::string::npos);
    assert(lines[0].find("\"level\":\"ERROR\"") != std::string::npos);
    assert(lines[0].find("\"order\":\"77\"") != std::string::npos);
    assert(lines[1].find("\"value\":12.5") != std::string::npos);
    assert(lines[1].find("orders.filled") != std::string::npos);

    pass("logging: JSON escaping, structured fields, telemetry on the same pipe");
}

void test_log_nothing_lost_at_shutdown()
{
    std::atomic<int> seen{0};
    {
        logger lg;
        lg.add_sink([&](const record&) { seen.fetch_add(1); });
        for (int i = 0; i < 500; ++i)
            SN_LOGGER_INFO(lg) << i;
        // No flush: the destructor must drain what is still queued
    }
    assert(seen.load() == 500);

    pass("logging: records queued at destruction are still delivered");
}

void test_log_moveable()
{
    // The library's signature, applied to a logger: build it configured in a
    // factory, move it into place, and it keeps running
    std::atomic<int> seen{0};
    auto make = [&] {
        logger lg;
        lg.add_sink([&](const record&) { seen.fetch_add(1); });
        lg.add_lane("second").add_sink([&](const record&) { seen.fetch_add(1); });
        return lg;
    };

    std::vector<logger> shelf;
    shelf.push_back(make());
    logger lg = std::move(shelf.back());
    shelf.clear();

    SN_LOGGER_INFO(lg) << "after the move";
    lg.flush();
    assert(seen.load() == 2);       // both lanes survived the move

    pass("logging: a configured logger moves, lanes and sinks intact");
}

} // namespace

void run_logging_tests()
{
    test_log_basics();
    test_log_level_filtering();
    test_log_many_threads();
    test_log_lanes_are_independent();
    test_log_overflow_counted();
    test_log_json_and_metrics();
    test_log_nothing_lost_at_shutdown();
    test_log_moveable();
}
