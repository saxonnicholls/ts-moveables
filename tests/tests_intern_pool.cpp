//
//  tests_intern_pool.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - unit tests for the content-addressed intern_pool
//

#include "test_helpers.hpp"

#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../TSMoveables/concurrent/intern_pool.hpp"

using namespace snicholls;

namespace {

// Equal values return the SAME pointer; different values return different ones.
void test_identity()
{
    intern_pool<std::string> pool;

    auto a1 = pool.intern(std::string("BILL_OF_LADING#42"));
    auto a2 = pool.intern(std::string("BILL_OF_LADING#42"));
    auto b  = pool.intern(std::string("BILL_OF_LADING#43"));

    assert(a1);
    assert(a1.get() == a2.get());     // identical data -> one pointer
    assert(*a1 == "BILL_OF_LADING#42");
    assert(a1.get() != b.get());      // distinct data -> distinct pointer
    assert(*b == "BILL_OF_LADING#43");

    const auto s = pool.snapshot();
    assert(s.interned == 3);
    assert(s.hits == 1);              // the second intern of #42 was a hit
    assert(s.live == 2);             // two distinct values alive
    pass("intern_pool: identical data collapses to one pointer, distinct stays distinct");
}

// A duplicate does not add a reference-count owner beyond the shared instance,
// and the buffer is stored once.
void test_refcount_and_sharing()
{
    intern_pool<std::string> pool;
    auto a1 = pool.intern(std::string("payload"));
    auto a2 = pool.intern(std::string("payload"));
    auto a3 = pool.intern(std::string("payload"));

    assert(a1.get() == a2.get() && a2.get() == a3.get());
    assert(a1.use_count() >= 3);      // three owners of the one buffer
    pass("intern_pool: duplicates share one buffer under multiple owners");
}

// Self-cleaning: when all owners drop, the entry is reclaimed and a later intern
// of the same value produces a fresh instance - live count stays bounded.
void test_weak_self_clean()
{
    intern_pool<std::string> pool;

    const void* first = nullptr;
    {
        auto a = pool.intern(std::string("ephemeral"));
        first = a.get();
        assert(pool.snapshot().live == 1);
    } // a drops here -> weak ref expires

    pool.sweep();
    assert(pool.snapshot().live == 0);          // reclaimed, no leak

    auto b = pool.intern(std::string("ephemeral"));
    assert(b.get() != first);                   // a genuinely new instance
    assert(pool.snapshot().live == 1);
    pass("intern_pool: weak refs self-clean; memory bounded by the live set");
}

// find() is a read-only probe: sees a live value, does not create one.
void test_find_probe()
{
    intern_pool<std::string> pool;
    assert(pool.find("nope") == nullptr);

    auto a = pool.intern(std::string("present"));
    auto f = pool.find("present");
    assert(f && f.get() == a.get());
    assert(pool.snapshot().live == 1);          // find created nothing
    pass("intern_pool: find() probes without allocating");
}

// Correctness under hash collisions: a hash that sends everything to one bucket
// must STILL keep distinct values distinct (equality is the authority, not the
// hash). This is the money/freight safety property.
void test_collision_safety()
{
    struct AllCollide {
        std::size_t operator()(const std::string&) const noexcept { return 0; }
    };
    intern_pool<std::string, AllCollide> pool;

    auto x = pool.intern(std::string("alpha"));
    auto y = pool.intern(std::string("beta"));
    auto x2 = pool.intern(std::string("alpha"));

    assert(x.get() != y.get());       // different values never alias, same bucket
    assert(x.get() == x2.get());      // same value still dedupes
    assert(*x == "alpha" && *y == "beta");
    assert(pool.snapshot().live == 2);
    pass("intern_pool: distinct values never alias even in one hash bucket");
}

// Concurrency: many threads interning from a small shared vocabulary all
// converge on the same canonical pointers; exactly the distinct values survive.
void test_concurrent_convergence()
{
    intern_pool<std::string> pool;
    const std::vector<std::string> vocab = {"LC", "eBL", "INVOICE", "SETTLE", "STATUS"};

    constexpr int threads = 8;
    constexpr int iters   = 5000;
    std::vector<std::thread> pool_threads;
    std::vector<std::vector<intern_pool<std::string>::ptr>> kept(threads);

    for (int t = 0; t < threads; ++t) {
        pool_threads.emplace_back([&, t] {
            for (int i = 0; i < iters; ++i)
                kept[t].push_back(pool.intern(std::string(vocab[(i + t) % vocab.size()])));
        });
    }
    for (auto& th : pool_threads)
        th.join();

    // For each vocab word, every owner across all threads must be the same ptr.
    for (const auto& word : vocab) {
        const void* canonical = pool.find(word).get();
        assert(canonical != nullptr);
        std::size_t seen = 0;
        for (int t = 0; t < threads; ++t)
            for (const auto& p : kept[t])
                if (*p == word) {
                    assert(p.get() == canonical);   // one pointer per value, always
                    ++seen;
                }
        assert(seen > 0);
    }

    const auto s = pool.snapshot();
    assert(s.live == vocab.size());                 // only the distinct values
    assert(s.interned == static_cast<std::uint64_t>(threads) * iters);
    assert(s.hits > 0);
    pass("intern_pool: concurrent interning converges to one pointer per value");
}


// The pool is a member of things, so it has to move - that is the library's
// whole argument. What must survive the move is the one guarantee the class
// makes: pointers handed out before the move stay canonical after it, so an
// equal value interned from the moved-to pool comes back pointer-identical.
void test_intern_pool_moves()
{
    snicholls::intern_pool<std::string> a;
    const auto before = a.intern(std::string("shared-payload"));
    const auto other  = a.intern(std::string("distinct"));

    snicholls::intern_pool<std::string> b = std::move(a);

    // The pointer predates the move and is still the canonical one
    const auto after = b.intern(std::string("shared-payload"));
    assert(after == before);
    assert(after.get() == before.get());
    assert(b.find(std::string("distinct")) == other);

    // Move-assignment too, and the table really did travel
    snicholls::intern_pool<std::string> c;
    c = std::move(b);
    assert(c.intern(std::string("shared-payload")) == before);
    assert(c.snapshot().live == 2);

    // A pool held BY a moveable object keeps that object's rule of zero - the
    // case that made non-movable wrong rather than merely inconsistent
    struct ingress {
        snicholls::intern_pool<std::string> pool;
        std::string name;
    };
    ingress one;
    const auto keep = one.pool.intern(std::string("via-member"));
    ingress two = std::move(one);
    assert(two.pool.intern(std::string("via-member")) == keep);

    pass("intern_pool: moves, and canonical pointers survive the move");
}

} // namespace

void run_intern_pool_tests()
{
    test_intern_pool_moves();
    test_identity();
    test_refcount_and_sharing();
    test_weak_self_clean();
    test_find_probe();
    test_collision_safety();
    test_concurrent_convergence();
}
