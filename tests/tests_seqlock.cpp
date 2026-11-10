//
//  tests_seqlock.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - unit tests for seqlock
//

#include "test_helpers.hpp"

#include <atomic>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "../TSMoveables/concurrent/seqlock.hpp"

using namespace snicholls;

namespace {

// Eight words, all written with the same stamp: a torn copy shows up as two
// different values in one read
struct stamped {
    std::uint64_t w[8];
};

stamped stamp(std::uint64_t k)
{
    stamped s{};
    for (auto& x : s.w)
        x = k;
    return s;
}

bool consistent(const stamped& s)
{
    for (auto x : s.w)
        if (x != s.w[0])
            return false;
    return true;
}

// Not a multiple of 8 bytes, mixed types, padding inside
struct odd_sized {
    std::uint16_t a;
    double b;
    char c[3];
};

// Trivially copyable but not default constructible would be rejected at
// compile time (read() returns a T); this is the positive case
struct quote {
    double bid;
    double ask;
    std::uint32_t bid_size;
    std::uint32_t ask_size;
};

void test_seqlock_basics()
{
    seqlock<quote> q{quote{100.0, 100.5, 10, 20}};
    const quote a = q.read();
    assert(a.bid == 100.0 && a.ask == 100.5 && a.bid_size == 10 && a.ask_size == 20);
    assert(q.version() == 0);

    q.write(quote{101.0, 101.25, 5, 7});
    const quote b = q.read();
    assert(b.bid == 101.0 && b.ask == 101.25 && b.bid_size == 5 && b.ask_size == 7);
    assert(q.version() == 1);

    quote c{};
    assert(q.try_read(c));                      // nobody is writing: one attempt suffices
    assert(c.bid == 101.0);

    const auto [value, version] = q.read_versioned();
    assert(value.ask == 101.25 && version == 1);

    seqlock<int> d;                             // default: a value-initialised T
    assert(d.read() == 0);
    pass("seqlock: write, read, try_read, read_versioned, version");
}

void test_seqlock_odd_sizes()
{
    seqlock<odd_sized> s{odd_sized{7, 2.5, {'x', 'y', 'z'}}};
    const odd_sized a = s.read();
    assert(a.a == 7 && a.b == 2.5 && a.c[0] == 'x' && a.c[2] == 'z');

    seqlock<char> one{'q'};                     // smaller than one word
    one.write('r');
    assert(one.read() == 'r');

    struct big { std::uint64_t v[33]; };        // more than four cache lines of words
    big init{};
    init.v[0] = 1;
    init.v[32] = 33;
    seqlock<big> large{init};
    assert(large.read().v[32] == 33);
    pass("seqlock: sizes that are not a multiple of 8 bytes, one byte, and many words");
}

void test_seqlock_move()
{
    seqlock<quote> a{quote{1, 2, 3, 4}};
    a.write(quote{5, 6, 7, 8});
    seqlock<quote> b = std::move(a);            // quiescent: the handle moves, the state stays put
    assert(b.read().bid == 5 && b.version() == 1);

    seqlock<quote> c;
    c = std::move(b);
    c.write(quote{9, 10, 11, 12});
    assert(c.read().ask == 10 && c.version() == 2);

    std::vector<seqlock<quote>> many;           // into a container, which is the point of moveability
    for (int i = 0; i < 16; ++i)
        many.emplace_back(quote{double(i), 0, 0, 0});
    assert(many[15].read().bid == 15);
    pass("seqlock: moves while quiescent, and lives in a std::vector");
}

// The property that matters: under a writer that never stops, every read is a
// whole value from ONE write - never torn - and a reader never sees the
// values go backwards.
void test_seqlock_concurrent()
{
    seqlock<stamped> s{stamp(0)};
    std::atomic<bool> stop{false};
    std::atomic<int> torn{0};
    std::atomic<int> backwards{0};
    std::atomic<long long> reads{0};

    std::thread writer([&] {
        for (std::uint64_t k = 1; !stop.load(std::memory_order_relaxed); ++k)
            s.write(stamp(k));
    });

    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r)
        readers.emplace_back([&] {
            std::uint64_t last = 0;
            long long n = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                const stamped v = s.read();
                if (!consistent(v))
                    torn.fetch_add(1, std::memory_order_relaxed);
                if (v.w[0] < last)
                    backwards.fetch_add(1, std::memory_order_relaxed);
                last = v.w[0];

                stamped t{};
                if (s.try_read(t) && !consistent(t))   // a successful try_read is whole too
                    torn.fetch_add(1, std::memory_order_relaxed);
                ++n;
            }
            reads.fetch_add(n, std::memory_order_relaxed);
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    for (auto& t : readers)
        t.join();

    assert(torn.load() == 0);
    assert(backwards.load() == 0);
    assert(reads.load() > 0);
    assert(s.version() > 0 && consistent(s.read()));
    pass("seqlock: one writer flat out, four readers - no torn copy, never backwards");
}

// The move check's detector, tested directly. A failed move cannot be provoked
// safely - if detection ever missed, the writer would be left writing into a
// moved-from handle - so the test asserts what the move relies on instead:
// quiet means quiescent, and a live writer is seen.
void test_seqlock_quiescence()
{
    static_assert(!std::is_copy_constructible_v<seqlock<int>>, "a seqlock has one owner of its state");
    static_assert(std::is_move_constructible_v<seqlock<int>> && std::is_move_assignable_v<seqlock<int>>,
                  "a seqlock moves");

    seqlock<stamped> s{stamp(0)};
    assert(s.quiescent());                      // nobody writing

    std::atomic<bool> stop{false};
    std::thread writer([&] {
        for (std::uint64_t k = 1; !stop.load(std::memory_order_relaxed); ++k)
            s.write(stamp(k));
    });
    bool seen = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!seen && std::chrono::steady_clock::now() < deadline)
        seen = !s.quiescent();
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    assert(seen);                               // a writer flat out is detected
    assert(s.quiescent());                      // and once it stops, quiet again

    seqlock<stamped> moved = std::move(s);      // quiescent: the move goes through
    assert(moved.quiescent() && consistent(moved.read()));
    pass("seqlock: quiescent() sees a live writer, and a quiet seqlock moves");
}

// version() is a cheap change test: it only grows, by one per write
void test_seqlock_versions()
{
    seqlock<int> s;
    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (int i = 1; i <= 100000; ++i)
            s.write(i);
        done.store(true, std::memory_order_release);
    });
    std::uint64_t last = 0;
    bool monotonic = true;
    while (!done.load(std::memory_order_acquire)) {
        const auto [value, version] = s.read_versioned();
        if (version < last || static_cast<std::uint64_t>(value) != version)
            monotonic = false;                  // value i is written by write number i
        last = version;
    }
    writer.join();
    assert(monotonic);
    assert(s.version() == 100000 && s.read() == 100000);
    pass("seqlock: read_versioned pairs each value with its write number");
}

} // namespace

void run_seqlock_tests()
{
    test_seqlock_basics();
    test_seqlock_odd_sizes();
    test_seqlock_move();
    test_seqlock_concurrent();
    test_seqlock_quiescence();
    test_seqlock_versions();
}
