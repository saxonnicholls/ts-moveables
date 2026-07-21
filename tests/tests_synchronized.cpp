//
//  tests_synchronized.cpp
//  TSMoveables
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - unit tests for synchronized<T>,
//  synchronized_waitable<T> and the heterogeneous containers
//

#include "test_helpers.hpp"

#include <any>
#include <chrono>
#include <queue>
#include <shared_mutex>
#include <string>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <variant>
#include <vector>

#include "../TSMoveables/ts_moveables.hpp"

using namespace snicholls;
using namespace std::chrono_literals;

namespace {

void test_synchronized()
{
    synchronized<int> s{41};
    s.with_lock([](int& v) { ++v; });
    assert(s.load() == 42);
    assert(s.with_lock([](const int& v) { return v * 2; }) == 84);

    const synchronized<int>& cs = s;
    assert(cs.with_lock([](const int& v) { return v; }) == 42);

    s = 7;                                  // operator= from T
    assert(s.exchange(9) == 7);
    assert(s.load() == 9);
    s.store(1);

    // Copy and move hold the source's lock; the mutex itself never moves
    synchronized<int> c(s);
    assert(c.load() == 1);
    synchronized<int> mv(std::move(s));
    assert(mv.load() == 1);
    s.store(5);                             // moved-from stays fully usable
    assert(s.load() == 5);
    c = mv;
    assert(c.load() == 1);
    c = std::move(mv);
    assert(c.load() == 1);

    // Contention - a plain long long made safe purely by the closure discipline
    constexpr int n_threads = 8;
    constexpr int n_iters = 10000;
    synchronized<long long> total{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < n_threads; ++t)
        threads.emplace_back([&] {
            for (int i = 0; i < n_iters; ++i)
                total.with_lock([](long long& v) { ++v; });
        });
    for (auto& t : threads)
        t.join();
    assert(total.load() == static_cast<long long>(n_threads) * n_iters);

    // Reader/writer via std::shared_mutex
    synchronized<int, std::shared_mutex> shared{10};
    assert(shared.with_read_lock([](const int& v) { return v; }) == 10);
    shared.with_write_lock([](int& v) { v = 11; });
    assert(shared.with_read_lock([](const int& v) { return v; }) == 11);

    // Backed by this library's spin lock
    synchronized<int, moveable_spin_lock> spins{1};
    spins.with_lock([](int& v) { ++v; });
    assert(spins.load() == 2);

    // Recursive mutex allows nested with_lock on the same object
    synchronized<int, std::recursive_mutex> rec{0};
    rec.with_lock([&](int& v) {
        rec.with_lock([](int& w) { ++w; });
        ++v;
    });
    assert(rec.load() == 2);

    // In a vector
    std::vector<synchronized<int>> v;
    for (int i = 0; i < 20; ++i)
        v.emplace_back(i);
    for (int i = 0; i < 20; ++i)
        assert(v[static_cast<std::size_t>(i)].load() == i);

    pass("synchronized");
}

void test_synchronized_waitable()
{
    // Producer/consumer with update / wait_then
    synchronized_waitable<std::queue<int>> q;
    long long sum = 0;
    std::thread consumer([&] {
        int received = 0;
        while (received < 100)
            q.wait_then([](const std::queue<int>& qu) { return !qu.empty(); },
                        [&](std::queue<int>& qu) {
                            while (!qu.empty()) {
                                sum += qu.front();
                                qu.pop();
                                ++received;
                            }
                        });
    });
    for (int i = 1; i <= 100; ++i)
        q.update([i](std::queue<int>& qu) { qu.push(i); });
    consumer.join();
    assert(sum == 5050);

    // wait / wait_for / wait_until
    synchronized_waitable<int> flag{0};
    assert(!flag.wait_for(5ms, [](int v) { return v == 1; }));
    flag.update([](int& v) { v = 1; });
    assert(flag.wait_for(1s, [](int v) { return v == 1; }));
    flag.wait([](int v) { return v == 1; });
    assert(flag.wait_until(std::chrono::steady_clock::now() + 5ms, [](int v) { return v == 1; }));

    // Moving while a thread waits throws; quiescent moves succeed
    synchronized_waitable<int> w{0};
    std::thread waiter([&] { w.wait([](int v) { return v == 42; }); });
    spin_until([&] { return w.waiting() > 0; });
    assert(throws_runtime_error([&] { synchronized_waitable<int> stolen(std::move(w)); }));
    assert(throws_runtime_error([&] { synchronized_waitable<int> target; target = std::move(w); }));
    w.update([](int& v) { v = 42; });
    waiter.join();
    assert(w.waiting() == 0);
    synchronized_waitable<int> moved(std::move(w));
    assert(moved.load() == 42);

    pass("synchronized_waitable");
}

void test_synchronized_heterogeneous()
{
    // Closed set of alternatives - synchronized_variant
    synchronized_variant<int, std::string> either{42};
    assert(either.holds<int>());
    assert(either.try_get<int>().value() == 42);
    assert(!either.try_get<std::string>().has_value());
    either = std::string("hello");          // assign an alternative directly
    assert(either.holds<std::string>());
    const auto len = either.visit([](const auto& x) -> std::size_t {
        if constexpr (std::is_same_v<std::decay_t<decltype(x)>, std::string>)
            return x.size();
        else
            return 0;
    });
    assert(len == 5);
    either.visit([](auto& x) {
        if constexpr (std::is_same_v<std::decay_t<decltype(x)>, std::string>)
            x += "!";
    });
    assert(either.try_get<std::string>().value() == "hello!");

    // Fixed bundle - synchronized_tuple
    synchronized_tuple<int, std::string, double> bundle{std::in_place, 1, "two", 3.0};
    assert(bundle.get<0>() == 1);
    assert(bundle.get<std::string>() == "two");
    bundle.set<0>(10);
    bundle.set<double>(2.5);
    assert(bundle.get<int>() == 10);
    const auto described = bundle.apply([](const int& i, const std::string& s, const double& d) {
        return s.size() + static_cast<std::size_t>(i) + static_cast<std::size_t>(d);
    });
    assert(described == 3 + 10 + 2);

    // One value of unknown type - synchronized_any
    synchronized_any cell;
    assert(!cell.has_value());
    cell = std::string("boxed");
    assert(cell.holds<std::string>());
    assert(cell.try_get<std::string>().value() == "boxed");
    assert(!cell.try_get<int>().has_value());
    cell.reset();
    assert(!cell.has_value());

    // At most one value per type - synchronized_type_map
    struct Config { int retries; };
    struct Stats  { long long hits; };
    synchronized_type_map ctx;
    ctx.put(Config{3});
    ctx.emplace<Stats>(Stats{0});
    ctx.put(std::string("name"));
    assert(ctx.size() == 3);
    assert(ctx.contains<Config>());
    assert(!ctx.contains<double>());
    assert(ctx.try_get<Config>().value().retries == 3);
    assert(ctx.with<Config>([](Config& c) { c.retries = 5; }));
    assert(ctx.try_get<Config>().value().retries == 5);
    assert(!ctx.with<double>([](double&) {}));
    assert(ctx.erase<std::string>());
    assert(!ctx.erase<std::string>());
    assert(ctx.size() == 2);

    // Concurrent read-modify-write through the type map
    constexpr int n_threads = 4;
    constexpr int n_iters = 5000;
    std::vector<std::thread> threads;
    for (int t = 0; t < n_threads; ++t)
        threads.emplace_back([&] {
            for (int i = 0; i < n_iters; ++i)
                ctx.with<Stats>([](Stats& s) { ++s.hits; });
        });
    for (auto& t : threads)
        t.join();
    assert(ctx.try_get<Stats>().value().hits == static_cast<long long>(n_threads) * n_iters);

    // Open, ordered bag - synchronized_bag
    synchronized_bag bag;
    bag.push(1);
    bag.push(std::string("x"));
    bag.push(3.14);
    bag.push(2);
    assert(bag.size() == 4);
    assert(bag.count<int>() == 2);
    assert(bag.count<std::string>() == 1);
    long long total = 0;
    assert(bag.for_each<int>([&](int& v) { total += v; v *= 10; }) == 2);
    assert(total == 3);
    const auto ints = bag.extract<int>();
    assert(ints.size() == 2 && ints[0] == 10 && ints[1] == 20);
    assert(bag.size() == 2);

    // The whole heterogeneous structure still moves
    synchronized_bag bag2(std::move(bag));
    assert(bag2.size() == 2);
    synchronized_type_map ctx2(std::move(ctx));
    assert(ctx2.contains<Stats>());

    pass("synchronized heterogeneous (variant / tuple / any / type map / bag)");
}

} // namespace

void run_synchronized_tests()
{
    test_synchronized();
    test_synchronized_waitable();
    test_synchronized_heterogeneous();
}
