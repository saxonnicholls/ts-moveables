//
//  tests_circular_buffer.cpp
//  TSMoveables
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - unit tests for the SPSC circular buffer
//

#include "test_helpers.hpp"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../TSMoveables/circular_buffer.hpp"

using namespace snicholls;

namespace {

// Every construction balanced by a destruction - catches slot lifetime bugs
struct Counted {
    static inline std::atomic<int> alive{0};
    int v{0};
    explicit Counted(int x = 0) : v(x) { ++alive; }
    Counted(const Counted& o) : v(o.v) { ++alive; }
    Counted(Counted&& o) noexcept : v(o.v) { ++alive; }
    Counted& operator=(const Counted&) = default;
    Counted& operator=(Counted&&) = default;
    ~Counted() { --alive; }
};

void test_ring_basics()
{
    static_assert(alignof(circular_buffer<int>) >= 64);
    static_assert(circular_buffer<int>::cache_line_size >= 64);

    circular_buffer<int> ring{100};
    assert(ring.capacity() == 128);         // rounded up to a power of two
    assert(ring.empty());
    assert(!ring.full());
    assert(ring.size() == 0);

    assert(ring.try_push(1));
    assert(ring.try_push(2));
    assert(ring.try_emplace(3));
    assert(ring.size() == 3);

    int out = 0;
    assert(ring.try_pop(out) && out == 1);  // FIFO
    auto opt = ring.try_pop();
    assert(opt && *opt == 2);
    assert(ring.try_pop(out) && out == 3);
    assert(!ring.try_pop(out));             // empty
    assert(!ring.try_pop().has_value());

    // Full behaviour - exact capacity, no wasted slot
    circular_buffer<int> tiny{4};
    assert(tiny.capacity() == 4);
    for (int i = 0; i < 4; ++i)
        assert(tiny.try_push(i));
    assert(tiny.full());
    assert(!tiny.try_push(99));
    assert(tiny.try_pop(out) && out == 0);
    assert(tiny.try_push(99));              // one slot freed, one accepted
    assert(!tiny.try_push(100));

    // Zero capacity refuses loudly
    bool threw = false;
    try {
        circular_buffer<int> bad{0};
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);

    pass("circular buffer basics");
}

void test_ring_wraparound()
{
    // Indices march monotonically; the mask does the wrapping. Cycle a small
    // ring many times its capacity to cover the wrap path.
    circular_buffer<int> ring{8};
    int out = 0;
    for (int cycle = 0; cycle < 1000; ++cycle) {
        for (int i = 0; i < 5; ++i)
            assert(ring.try_push(cycle * 5 + i));
        for (int i = 0; i < 5; ++i) {
            assert(ring.try_pop(out));
            assert(out == cycle * 5 + i);
        }
    }
    assert(ring.empty());

    pass("circular buffer wrap-around");
}

void test_ring_batch()
{
    circular_buffer<int> ring{8};
    std::vector<int> in(6);
    std::iota(in.begin(), in.end(), 0);

    assert(ring.push_n(in.begin(), in.size()) == 6);
    assert(ring.size() == 6);
    assert(ring.push_n(in.begin(), in.size()) == 2);    // partial: only 2 slots free
    assert(ring.full());

    std::vector<int> out;
    assert(ring.pop_n(std::back_inserter(out), 5) == 5);
    assert((out == std::vector<int>{0, 1, 2, 3, 4}));
    assert(ring.pop_n(std::back_inserter(out), 100) == 3);  // partial: only 3 left
    assert((out == std::vector<int>{0, 1, 2, 3, 4, 5, 0, 1}));
    assert(ring.pop_n(std::back_inserter(out), 100) == 0);

    // Moving batch via move_iterator
    circular_buffer<std::string> sring{4};
    std::vector<std::string> strs{"a", "b", "c"};
    assert(sring.push_n(std::make_move_iterator(strs.begin()), strs.size()) == 3);
    std::vector<std::string> sout;
    assert(sring.pop_n(std::back_inserter(sout), 3) == 3);
    assert((sout == std::vector<std::string>{"a", "b", "c"}));

    pass("circular buffer batch push/pop");
}

void test_ring_lifetimes()
{
    // Constructions and destructions balance exactly, including the
    // destructor draining what is left
    {
        circular_buffer<Counted> ring{8};
        for (int i = 0; i < 6; ++i)
            assert(ring.try_emplace(i));
        Counted out;
        assert(ring.try_pop(out));
        assert(ring.try_pop(out));
        ring.clear();
        assert(ring.empty());
        for (int i = 0; i < 3; ++i)
            assert(ring.try_emplace(i));
    }                                       // 3 still queued - drained by the destructor
    assert(Counted::alive.load() == 0);

    // Move-only element type
    circular_buffer<std::unique_ptr<int>> uring{4};
    assert(uring.try_push(std::make_unique<int>(7)));
    auto p = uring.try_pop();
    assert(p && **p == 7);

    pass("circular buffer element lifetimes");
}

void test_ring_spsc_threaded()
{
    // One producer, one consumer, capacity far smaller than the item count so
    // the ring wraps constantly and both sides contend
    constexpr int total = 100000;
    circular_buffer<int> ring{64};

    std::thread producer([&] {
        for (int i = 0; i < total;) {
            if (ring.try_push(i))
                ++i;
            else
                std::this_thread::yield();
        }
    });

    long long sum = 0;
    bool ordered = true;
    for (int received = 0; received < total;) {
        if (auto v = ring.try_pop()) {
            if (*v != received)
                ordered = false;
            sum += *v;
            ++received;
        } else {
            std::this_thread::yield();
        }
    }
    producer.join();
    assert(ordered);                        // strict FIFO
    assert(sum == static_cast<long long>(total) * (total - 1) / 2);
    assert(ring.empty());

    pass("circular buffer SPSC threaded");
}

void test_ring_spsc_threaded_batch()
{
    // Producer pushes in batches, consumer drains in batches - exercises the
    // amortised paths against each other
    constexpr int total = 100000;
    constexpr int batch = 16;
    circular_buffer<int> ring{64};

    std::thread producer([&] {
        std::vector<int> chunk(batch);
        int next = 0;
        while (next < total) {
            const int n = std::min(batch, total - next);
            std::iota(chunk.begin(), chunk.begin() + n, next);
            std::size_t pushed = 0;
            while (pushed < static_cast<std::size_t>(n)) {
                pushed += ring.push_n(chunk.begin() + static_cast<std::ptrdiff_t>(pushed),
                                      static_cast<std::size_t>(n) - pushed);
                if (pushed < static_cast<std::size_t>(n))
                    std::this_thread::yield();
            }
            next += n;
        }
    });

    std::vector<int> received;
    received.reserve(total);
    while (received.size() < static_cast<std::size_t>(total)) {
        if (ring.pop_n(std::back_inserter(received), 32) == 0)
            std::this_thread::yield();
    }
    producer.join();
    assert(received.size() == static_cast<std::size_t>(total));
    for (int i = 0; i < total; ++i)
        assert(received[static_cast<std::size_t>(i)] == i);

    pass("circular buffer SPSC threaded (batched)");
}

void test_ring_move_copy()
{
    // Move transfers the queued elements in order; the source is left empty,
    // with its capacity and storage intact and fully usable
    circular_buffer<std::string> a{8};
    assert(a.try_push("one"));
    assert(a.try_push("two"));
    assert(a.try_push("three"));

    circular_buffer<std::string> b(std::move(a));
    assert(b.size() == 3 && a.empty());
    assert(a.capacity() == 8);
    assert(a.try_push("fresh"));            // moved-from stays usable
    assert(*a.try_pop() == "fresh");

    // Copy is a snapshot; the source keeps its contents
    circular_buffer<std::string> c(b);
    assert(c.size() == 3 && b.size() == 3);
    for (const char* expected : {"one", "two", "three"})
        assert(*c.try_pop() == expected);
    assert(b.size() == 3);

    // Move assignment adopts the source's capacity
    circular_buffer<std::string> d{32};
    d = std::move(b);
    assert(d.capacity() == 8 && d.size() == 3);
    for (const char* expected : {"one", "two", "three"})
        assert(*d.try_pop() == expected);
    assert(b.empty() && b.capacity() == 8);

    // Copy assignment
    circular_buffer<std::string> e{4};
    assert(e.try_push("stale"));
    e = d;                                  // d is empty now
    assert(e.empty() && e.capacity() == 8);

    pass("circular buffer move/copy");
}

void test_ring_static_capacity()
{
    // Compile-time capacity: zero allocation, same algorithm
    circular_buffer<std::string, 4> ring;
    assert(ring.capacity() == 4);
    assert(ring.try_push("a"));
    assert(ring.try_push("b"));
    assert(ring.try_push("c"));
    assert(ring.try_push("d"));
    assert(ring.full());
    assert(!ring.try_push("e"));
    assert(*ring.try_pop() == "a");

    // Moves work the same way - so it lives happily in a vector
    std::vector<circular_buffer<int, 8>> rings;
    for (int i = 0; i < 10; ++i) {
        rings.emplace_back();
        assert(rings.back().try_push(i));
    }
    for (int i = 0; i < 10; ++i) {
        auto v = rings[static_cast<std::size_t>(i)].try_pop();
        assert(v && *v == i);
    }

    pass("circular buffer compile-time capacity");
}

} // namespace

void run_circular_buffer_tests()
{
    test_ring_basics();
    test_ring_wraparound();
    test_ring_batch();
    test_ring_lifetimes();
    test_ring_spsc_threaded();
    test_ring_spsc_threaded_batch();
    test_ring_move_copy();
    test_ring_static_capacity();
}
