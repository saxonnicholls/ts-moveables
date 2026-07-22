//
//  tests.cpp
//  TSMoveables
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - unit tests for the moveable primitives, plus main
//
//  Deliberately lightweight: cassert only, no test framework.
//  Build and run with `make test` from the repository root.
//

#include "test_helpers.hpp"

#include <chrono>
#include <shared_mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "../TSMoveables/ts_moveables.hpp"

// Defined in the per-area test translation units
void run_synchronized_tests();
void run_circular_buffer_tests();
void run_disruptor_tests();
void run_signal_tests();
void run_mpmc_queue_tests();
void run_thread_pool_tests();

using namespace snicholls;
using namespace std::chrono_literals;

// Some libstdc++ versions disable their pthread clocklock path under TSan but
// leave the steady_clock try_lock_until overloads of the timed mutexes calling
// it (GCC bug 113327), so those overloads fail to compile. Use system_clock
// deadlines for the timed-mutex tests there; every other build uses
// steady_clock as normal.
#if defined(_GLIBCXX_TSAN)
using deadline_clock = std::chrono::system_clock;
#else
using deadline_clock = std::chrono::steady_clock;
#endif

namespace {

// The constexpr constructors make statics of these types constant-initialized;
// constinit (C++20) is the compiler-enforced proof
#if defined(__cpp_constinit)
constinit moveable_atomic_int constinit_counter{7};
constinit moveable_atomic_flag constinit_flag{true};
constinit moveable_spin_lock constinit_lock;
#endif

// ------------------------------------------------- constexpr and noexcept ---

void test_constexpr_and_noexcept()
{
    static_assert(moveable_atomic_int::is_always_lock_free);
    static_assert(moveable_atomic_bool::is_always_lock_free);

    // try_lock and unlock throw nothing, as the standard requires of every
    // standard mutex; lock may throw std::system_error and stays unmarked
    static_assert(noexcept(std::declval<moveable_mutex<>&>().try_lock()));
    static_assert(noexcept(std::declval<moveable_mutex<>&>().unlock()));
    static_assert(!noexcept(std::declval<moveable_mutex<>&>().lock()));
    static_assert(noexcept(std::declval<moveable_shared_mutex&>().unlock_shared()));
    static_assert(noexcept(std::declval<moveable_spin_lock&>().lock()));

#if defined(__cpp_constinit)
    assert(constinit_counter.get() == 7);
    assert(constinit_flag.test());
    constinit_lock.lock();
    constinit_lock.unlock();
#endif

    pass("constexpr construction and noexcept guarantees");
}

// ---------------------------------------------------------- zero overhead ---

void test_zero_size_overhead()
{
    // The composition wrappers add nothing to the primitive they wrap -
    // this backs the "no overhead" claim in the README
    static_assert(sizeof(moveable_atomic<int>) == sizeof(std::atomic<int>));
    static_assert(sizeof(moveable_atomic<double>) == sizeof(std::atomic<double>));
    static_assert(sizeof(moveable_atomic_uint64_t) == sizeof(std::atomic<std::uint64_t>));
    static_assert(sizeof(moveable_atomic_flag) == sizeof(std::atomic<bool>));
    static_assert(sizeof(moveable_mutex<>) == sizeof(std::mutex));
    static_assert(sizeof(moveable_recursive_mutex) == sizeof(std::recursive_mutex));
    static_assert(sizeof(moveable_timed_mutex) == sizeof(std::timed_mutex));
    static_assert(sizeof(moveable_shared_mutex) == sizeof(std::shared_mutex));
    static_assert(sizeof(moveable_spin_lock) == sizeof(std::atomic<bool>));

    pass("zero size overhead (composition wrappers)");
}

// ---------------------------------------------------------------- atomics ---

void test_atomic_basics()
{
    moveable_atomic_int a{42};
    assert(a.get() == 42);
    assert(a.load() == 42);

    a.store(7);
    assert(a == 7);                         // operator T

    assert(a.exchange(9) == 7);
    assert(a.get() == 9);

    int expected = 9;
    assert(a.compare_exchange_strong(expected, 11));
    assert(a.get() == 11);
    expected = 999;
    assert(!a.compare_exchange_strong(expected, 0));
    assert(expected == 11);

    a = 100;                                // operator= from T
    assert(a.fetch_add(5) == 100);
    assert(a.fetch_sub(5) == 105);
    assert(a.get() == 100);
    assert(++a == 101);
    assert(a++ == 101);
    assert(a.get() == 102);
    assert(--a == 101);
    assert((a += 9) == 110);
    assert((a &= 0xFF) == 110);
    assert((a |= 0x01) == 111);
    assert((a ^= 0x02) == 109);

    moveable_atomic<unsigned> bits{0b1100};
    assert(bits.fetch_and(0b1010) == 0b1100);
    assert(bits.get() == 0b1000);
    assert(bits.fetch_or(0b0001) == 0b1000);
    assert(bits.fetch_xor(0b1001) == 0b1001);
    assert(bits.get() == 0b0000);

    (void)a.is_lock_free();

    // Non-integral specialisation still works for the value operations
    moveable_atomic<double> d{1.5};
    assert(d.get() == 1.5);
    d.store(2.5);
    assert(d.load() == 2.5);

    pass("atomic basics");
}

void test_atomic_copy_move()
{
    moveable_atomic_int a{42};

    moveable_atomic_int b(a);               // copy construct
    assert(b.get() == 42);

    moveable_atomic_int c = a;              // copy initialise (was broken by explicit)
    assert(c.get() == 42);

    moveable_atomic_int d(std::move(a));    // move construct
    assert(d.get() == 42);
    assert(a.get() == 42);                  // source unchanged by design

    moveable_atomic_int e;
    e = b;                                  // copy assign
    assert(e.get() == 42);
    e = 5;
    e = std::move(b);                       // move assign
    assert(e.get() == 42);

    std::atomic<int> raw{17};
    moveable_atomic_int f{raw};             // construct from std::atomic
    assert(f.get() == 17);

    static_assert(std::is_copy_constructible_v<moveable_atomic_int>);
    static_assert(std::is_move_constructible_v<moveable_atomic_int>);
    static_assert(std::is_copy_assignable_v<moveable_atomic_int>);
    static_assert(std::is_move_assignable_v<moveable_atomic_int>);

    pass("atomic copy/move");
}

void test_atomic_threaded()
{
    constexpr int n_threads = 8;
    constexpr int n_iters = 10000;

    moveable_atomic_int counter{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < n_threads; ++t)
        threads.emplace_back([&] {
            for (int i = 0; i < n_iters; ++i)
                ++counter;
        });
    for (auto& t : threads)
        t.join();
    assert(counter.get() == n_threads * n_iters);

    pass("atomic threaded increment");
}

void test_atomic_in_container()
{
    // The whole point: structs full of atomics get compiler-generated
    // copy/move and can live in vectors
    struct Stats {
        moveable_atomic_uint64_t hits{0};
        moveable_atomic_bool     enabled{true};
    };
    static_assert(std::is_move_constructible_v<Stats>);
    static_assert(std::is_copy_constructible_v<Stats>);

    std::vector<Stats> v;
    for (int i = 0; i < 100; ++i) {         // Forces reallocations
        v.emplace_back();
        v.back().hits.store(static_cast<std::uint64_t>(i));
    }
    for (int i = 0; i < 100; ++i)
        assert(v[static_cast<std::size_t>(i)].hits.get() == static_cast<std::uint64_t>(i));

    pass("atomic in vector");
}

void test_atomic_flag()
{
    moveable_atomic_flag f;
    assert(!f.test());
    assert(!f.test_and_set());              // returns previous value
    assert(f.test());
    assert(f.test_and_set());
    f.clear();
    assert(!f.test());

    moveable_atomic_flag set_flag{true};
    moveable_atomic_flag copied(set_flag);
    assert(copied.test());
    moveable_atomic_flag moved(std::move(set_flag));
    assert(moved.test());

    moveable_atomic_flag assigned;
    assigned = moved;
    assert(assigned.test());

    pass("atomic flag");
}

// ---------------------------------------------------------------- mutexes ---

void test_mutex_basics()
{
    moveable_mutex<> m;
    m.lock();
    m.unlock();
    assert(m.try_lock());
    m.unlock();

    {
        std::lock_guard< moveable_mutex<> > lock(m);
    }
    {
        std::unique_lock< moveable_mutex<> > lock(m);
        assert(lock.owns_lock());
    }

    pass("mutex basics");
}

void test_mutex_exclusion()
{
    constexpr int n_threads = 8;
    constexpr int n_iters = 10000;

    moveable_mutex<> m;
    long long counter = 0;                  // Deliberately not atomic
    std::vector<std::thread> threads;
    for (int t = 0; t < n_threads; ++t)
        threads.emplace_back([&] {
            for (int i = 0; i < n_iters; ++i) {
                std::lock_guard< moveable_mutex<> > lock(m);
                ++counter;
            }
        });
    for (auto& t : threads)
        t.join();
    assert(counter == static_cast<long long>(n_threads) * n_iters);

    pass("mutex mutual exclusion");
}

void test_mutex_move()
{
    // Moving an unlocked mutex succeeds
    moveable_mutex<> a;
    moveable_mutex<> b(std::move(a));
    b.lock();
    b.unlock();
    a.lock();                               // moved-from mutex remains usable
    a.unlock();

    moveable_mutex<> c;
    c = std::move(b);
    c.lock();
    c.unlock();

    // Moving a mutex locked by another thread throws
    moveable_mutex<> locked;
    moveable_atomic_bool release{false};
    std::thread holder([&] {
        locked.lock();
        spin_until([&] { return release.get(); });
        locked.unlock();
    });
    // Wait until the holder actually owns it
    spin_until([&] {
        if (locked.try_lock()) {
            locked.unlock();
            return false;
        }
        return true;
    });

    assert(throws_runtime_error([&] { moveable_mutex<> stolen(std::move(locked)); }));
    assert(throws_runtime_error([&] { moveable_mutex<> target; target = std::move(locked); }));

    release = true;
    holder.join();

    // After the holder released it, the move succeeds
    moveable_mutex<> fine(std::move(locked));
    fine.lock();
    fine.unlock();

    pass("mutex move semantics");
}

void test_mutex_variants()
{
    // Recursive
    moveable_recursive_mutex rm;
    rm.lock();
    rm.lock();
    assert(rm.try_lock());
    rm.unlock();
    rm.unlock();
    rm.unlock();

    // Timed
    moveable_timed_mutex tm;
    assert(tm.try_lock_for(10ms));
    tm.unlock();
    assert(tm.try_lock_until(deadline_clock::now() + 10ms));
    tm.unlock();

    // Shared - two simultaneous readers
    moveable_shared_mutex sm;
    sm.lock_shared();
    assert(sm.try_lock_shared());
    assert(!sm.try_lock());                 // writer blocked while readers hold it
    sm.unlock_shared();
    sm.unlock_shared();
    {
        std::shared_lock< moveable_shared_mutex > reader(sm);
        assert(reader.owns_lock());
    }
    sm.lock();
    sm.unlock();

    // Shared timed
    moveable_shared_timed_mutex stm;
    assert(stm.try_lock_shared_for(10ms));
    stm.unlock_shared();
    assert(stm.try_lock_shared_until(deadline_clock::now() + 10ms));
    stm.unlock_shared();

    // A shared-locked mutex refuses to move
    moveable_shared_mutex held;
    held.lock_shared();
    assert(throws_runtime_error([&] { moveable_shared_mutex stolen(std::move(held)); }));
    held.unlock_shared();

    pass("mutex variants (recursive/timed/shared)");
}

void test_mutex_in_container()
{
    struct Guarded {
        moveable_mutex<> m;
        int value{0};
    };
    std::vector<Guarded> v;
    for (int i = 0; i < 50; ++i) {          // Forces reallocations while unlocked
        v.emplace_back();
        v.back().value = i;
    }
    for (int i = 0; i < 50; ++i) {
        std::lock_guard< moveable_mutex<> > lock(v[static_cast<std::size_t>(i)].m);
        assert(v[static_cast<std::size_t>(i)].value == i);
    }

    pass("mutex in vector");
}

// -------------------------------------------------------------- spin lock ---

void test_spin_lock()
{
    moveable_spin_lock s;
    s.lock();
    assert(!s.try_lock());
    s.unlock();
    assert(s.try_lock());
    s.unlock();
    {
        std::lock_guard<moveable_spin_lock> lock(s);
    }
    {
        std::unique_lock<moveable_spin_lock> lock(s);
        assert(lock.owns_lock());
    }

    // Mutual exclusion
    constexpr int n_threads = 8;
    constexpr int n_iters = 10000;
    long long counter = 0;                  // Deliberately not atomic
    std::vector<std::thread> threads;
    for (int t = 0; t < n_threads; ++t)
        threads.emplace_back([&] {
            for (int i = 0; i < n_iters; ++i) {
                std::lock_guard<moveable_spin_lock> lock(s);
                ++counter;
            }
        });
    for (auto& t : threads)
        t.join();
    assert(counter == static_cast<long long>(n_threads) * n_iters);

    // Moving an unheld lock succeeds; the source stays usable
    moveable_spin_lock a;
    moveable_spin_lock b(std::move(a));
    b.lock();
    b.unlock();
    a.lock();
    a.unlock();
    moveable_spin_lock c;
    c = std::move(b);
    c.lock();
    c.unlock();

    // Moving a held lock throws
    moveable_spin_lock held;
    held.lock();
    assert(throws_runtime_error([&] { moveable_spin_lock stolen(std::move(held)); }));
    assert(throws_runtime_error([&] { moveable_spin_lock target; target = std::move(held); }));
    held.unlock();

    // In a vector
    struct Guarded {
        moveable_spin_lock lock;
        int value{0};
    };
    std::vector<Guarded> v;
    for (int i = 0; i < 50; ++i) {
        v.emplace_back();
        v.back().value = i;
    }
    for (int i = 0; i < 50; ++i) {
        std::lock_guard<moveable_spin_lock> lock(v[static_cast<std::size_t>(i)].lock);
        assert(v[static_cast<std::size_t>(i)].value == i);
    }

    pass("spin lock");
}

// --------------------------------------------------- condition variables ---

void test_condition_variable()
{
    moveable_mutex<> m;
    moveable_condition_variable<> cv;
    bool ready = false;
    bool processed = false;

    std::thread worker([&] {
        std::unique_lock<std::mutex> lock(m.native());
        cv.wait(lock, [&] { return ready; });
        processed = true;
        lock.unlock();
        cv.notify_all();
    });

    spin_until([&] { return cv.waiting() > 0; });

    // Moving while a thread waits throws
    assert(throws_runtime_error([&] { moveable_condition_variable<> stolen(std::move(cv)); }));
    assert(throws_runtime_error([&] { moveable_condition_variable<> target; target = std::move(cv); }));

    {
        std::lock_guard<std::mutex> lock(m.native());
        ready = true;
    }
    cv.notify_one();
    {
        std::unique_lock<std::mutex> lock(m.native());
        cv.wait(lock, [&] { return processed; });
    }
    worker.join();
    assert(processed);
    assert(cv.waiting() == 0);

    // Quiescent now - moving succeeds and the target works
    moveable_condition_variable<> cv2(std::move(cv));
    {
        std::unique_lock<std::mutex> lock(m.native());
        assert(cv2.wait_for(lock, 1ms, [] { return true; }));
        assert(cv2.wait_until(lock, std::chrono::steady_clock::now() + 1ms,
                              [] { return true; }));
    }

    pass("condition variable");
}

void test_condition_variable_any()
{
    // condition_variable_any waits directly on a moveable_mutex
    moveable_mutex<> m;
    moveable_condition_variable_any cv;
    bool ready = false;

    std::thread worker([&] {
        std::unique_lock< moveable_mutex<> > lock(m);
        cv.wait(lock, [&] { return ready; });
    });

    spin_until([&] { return cv.waiting() > 0; });
    {
        std::lock_guard< moveable_mutex<> > lock(m);
        ready = true;
    }
    cv.notify_all();
    worker.join();

    moveable_condition_variable_any cv2(std::move(cv));
    (void)cv2;

    pass("condition variable any");
}

// -------------------------------------------------------------- once flag ---

void test_once_flag()
{
    moveable_once_flag flag;
    assert(!flag.called());

    moveable_atomic_int runs{0};
    constexpr int n_threads = 8;
    std::vector<std::thread> threads;
    for (int t = 0; t < n_threads; ++t)
        threads.emplace_back([&] { call_once(flag, [&] { ++runs; }); });
    for (auto& t : threads)
        t.join();
    assert(runs.get() == 1);
    assert(flag.called());

    // Copy and move both preserve the "already ran" state
    moveable_once_flag copied(flag);
    assert(copied.called());
    copied.call_once([&] { ++runs; });
    assert(runs.get() == 1);                // Does not run again

    moveable_once_flag moved(std::move(flag));
    assert(moved.called());

    moveable_once_flag assigned;
    assigned = moved;
    assert(assigned.called());

    // A throwing callable does not consume the flag - matches std::call_once
    moveable_once_flag retry;
    bool threw = false;
    try {
        retry.call_once([] { throw std::logic_error("boom"); });
    } catch (const std::logic_error&) {
        threw = true;
    }
    assert(threw);
    assert(!retry.called());
    retry.call_once([] {});
    assert(retry.called());

    // Arguments are forwarded
    moveable_once_flag args_flag;
    int target = 0;
    args_flag.call_once([](int& t, int v) { t = v; }, target, 99);
    assert(target == 99);

    pass("once flag");
}

// -------------------------------------------------------------- semaphore ---

void test_semaphore()
{
    moveable_semaphore sem{2};
    assert(sem.available() == 2);
    sem.acquire();
    assert(sem.try_acquire());
    assert(!sem.try_acquire());             // Exhausted
    assert(!sem.try_acquire_for(1ms));
    sem.release(2);
    assert(sem.available() == 2);
    assert(sem.try_acquire_for(1ms));
    assert(sem.try_acquire_until(std::chrono::steady_clock::now() + 1ms));
    sem.release(2);

    // Blocked acquirer is woken by release
    moveable_semaphore gate{0};
    moveable_atomic_bool acquired{false};
    std::thread waiter([&] {
        gate.acquire();
        acquired = true;
    });
    gate.release();
    spin_until([&] { return acquired.get(); });
    waiter.join();

    // Semaphore as a mutual-exclusion limiter
    moveable_semaphore limiter{3};
    moveable_atomic_int in_flight{0};
    moveable_atomic_bool over_limit{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
        threads.emplace_back([&] {
            for (int i = 0; i < 200; ++i) {
                limiter.acquire();
                if (++in_flight > 3)
                    over_limit = true;
                --in_flight;
                limiter.release();
            }
        });
    for (auto& t : threads)
        t.join();
    assert(!over_limit.get());

    // Move transfers the count and zeroes the source
    moveable_semaphore src{5};
    moveable_semaphore dst(std::move(src));
    assert(dst.available() == 5);
    assert(src.available() == 0);
    moveable_semaphore dst2;
    dst2 = std::move(dst);
    assert(dst2.available() == 5);
    assert(dst.available() == 0);

    pass("semaphore");
}

// ------------------------------------------------------------------ latch ---

void test_latch()
{
    constexpr int n_workers = 6;
    moveable_latch latch{n_workers};
    assert(!latch.try_wait());
    assert(latch.remaining() == n_workers);

    std::vector<std::thread> threads;
    for (int t = 0; t < n_workers; ++t)
        threads.emplace_back([&] { latch.count_down(); });
    latch.wait();                           // Blocks until all have counted down
    assert(latch.try_wait());
    assert(latch.remaining() == 0);
    for (auto& t : threads)
        t.join();

    // arrive_and_wait: all workers rendezvous
    moveable_latch rendezvous{3};
    moveable_atomic_int through{0};
    threads.clear();
    for (int t = 0; t < 3; ++t)
        threads.emplace_back([&] {
            rendezvous.arrive_and_wait();
            ++through;
        });
    for (auto& t : threads)
        t.join();
    assert(through.get() == 3);

    // Counting below zero throws instead of undefined behaviour
    moveable_latch tiny{1};
    bool threw = false;
    try {
        tiny.count_down(2);
    } catch (const std::logic_error&) {
        threw = true;
    }
    assert(threw);
    assert(tiny.remaining() == 1);

    // Move transfers the remaining count
    moveable_latch src{4};
    src.count_down();
    moveable_latch dst(std::move(src));
    assert(dst.remaining() == 3);
    assert(src.try_wait());                 // Source is left released
    moveable_latch dst2;
    dst2 = std::move(dst);
    assert(dst2.remaining() == 3);

    pass("latch");
}

// ---------------------------------------------------------------- barrier ---

void test_barrier()
{
    constexpr int n_threads = 4;
    constexpr int n_phases = 5;

    moveable_atomic_int completions{0};
    moveable_barrier barrier{n_threads, [&completions]() noexcept { ++completions; }};

    // Each phase: all threads bump a per-phase counter, then rendezvous;
    // after the barrier every thread must observe the full count.
    moveable_atomic_int phase_counter{0};
    moveable_atomic_bool mismatch{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < n_threads; ++t)
        threads.emplace_back([&] {
            for (int p = 0; p < n_phases; ++p) {
                ++phase_counter;
                barrier.arrive_and_wait();
                if (phase_counter.get() < n_threads * (p + 1))
                    mismatch = true;
                barrier.arrive_and_wait();  // Second rendezvous before next phase
            }
        });
    for (auto& t : threads)
        t.join();
    assert(!mismatch.get());
    assert(completions.get() == n_phases * 2);
    assert(barrier.phase() == n_phases * 2);

    // arrive_and_drop reduces the expected count for subsequent phases
    moveable_barrier<> b2{2};
    std::thread dropper([&] { b2.arrive_and_drop(); });
    b2.arrive_and_wait();                   // Completes the phase with the dropper
    dropper.join();
    b2.arrive_and_wait();                   // Now a 1-thread barrier - returns at once
    assert(b2.phase() == 2);

    // Move transfers configuration and phase while quiescent
    moveable_barrier<> src{1};
    src.arrive_and_wait();
    moveable_barrier<> dst(std::move(src));
    assert(dst.phase() == 1);
    dst.arrive_and_wait();
    assert(dst.phase() == 2);
    moveable_barrier<> dst2;
    dst2 = std::move(dst);
    assert(dst2.phase() == 2);

    pass("barrier");
}

// ----------------------------------------------------------- integration ---

void test_everything_in_one_object()
{
    // The library's reason to exist: a struct using every primitive still gets
    // compiler-generated moves and can be moved and stored freely.
    struct Everything {
        moveable_atomic_int             counter{0};
        moveable_atomic_flag            flag;
        moveable_mutex<>                mutex;
        moveable_spin_lock              spin;
        moveable_shared_mutex           shared_mutex;
        moveable_condition_variable<>   cv;
        moveable_condition_variable_any cv_any;
        moveable_once_flag              once;
        synchronized<int>               sync{0};
        synchronized_waitable<int>      sync_waitable{0};
        circular_buffer<int>            ring{8};
        circular_buffer<int, 4>         static_ring;
        disruptor<int>                  events{8};
        moveable_signal<int>            changed;
        moveable_semaphore              semaphore{1};
        moveable_latch                  latch{1};
        moveable_barrier<>              barrier{1};
    };
    static_assert(std::is_move_constructible_v<Everything>);
    static_assert(std::is_move_assignable_v<Everything>);
    static_assert(!std::is_copy_constructible_v<Everything>);   // mutex et al. are move-only

    Everything e1;
    e1.counter = 41;
    e1.once.call_once([&] { ++e1.counter; });

    Everything e2(std::move(e1));
    assert(e2.counter.get() == 42);
    assert(e2.once.called());
    e2.mutex.lock();
    e2.mutex.unlock();
    e2.semaphore.acquire();
    e2.semaphore.release();
    e2.latch.count_down();
    e2.latch.wait();
    e2.barrier.arrive_and_wait();

    Everything e3;
    e3 = std::move(e2);
    assert(e3.counter.get() == 42);

    std::vector<Everything> v;
    for (int i = 0; i < 10; ++i)
        v.emplace_back();                   // Reallocation exercises all the moves
    assert(v.size() == 10);

    pass("everything in one moveable object");
}

} // namespace

int main()
{
    test_constexpr_and_noexcept();
    test_zero_size_overhead();

    test_atomic_basics();
    test_atomic_copy_move();
    test_atomic_threaded();
    test_atomic_in_container();
    test_atomic_flag();

    test_mutex_basics();
    test_mutex_exclusion();
    test_mutex_move();
    test_mutex_variants();
    test_mutex_in_container();

    test_spin_lock();

    test_condition_variable();
    test_condition_variable_any();

    run_synchronized_tests();
    run_circular_buffer_tests();
    run_disruptor_tests();
    run_signal_tests();
    run_mpmc_queue_tests();
    run_thread_pool_tests();

    test_once_flag();

    test_semaphore();
    test_latch();
    test_barrier();

    test_everything_in_one_object();

    std::cout << "\nAll " << tests_run << " tests passed\n";
    return 0;
}
