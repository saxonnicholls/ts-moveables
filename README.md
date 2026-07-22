# ts-moveables
Thread Safe Moveable Objects

[![CI](https://github.com/saxonnicholls/ts-moveables/actions/workflows/ci.yml/badge.svg)](https://github.com/saxonnicholls/ts-moveables/actions/workflows/ci.yml)

We often need to move, so called "immovable" objects in C++ such as atomics, mutexes and condition variables. This library provides portable mechanisms to do this — and the same discipline, applied consistently, has grown into a small header-only concurrency toolkit:

- **[the moveable primitives](#the-types)** — atomic, the full mutex family, spin lock, condition variable, once flag, semaphore, latch, barrier: every "immovable" synchronisation type, moveable with its state integrity intact
- **[`synchronized<T>`](#synchronizedt)** — a value bonded to its mutex, reachable only under the lock, plus ready-made thread-safe heterogeneous containers (variant / tuple / any / type map / bag)
- **[`circular_buffer`](#circular_buffer)** — a wait-free SPSC ring with two honest cache-line-separated atomics; ~2 ns/op batched
- **[`disruptor`](#disruptor)** — the LMAX pattern: pre-allocated events, consumer dependency graphs, batch consumption; ~1 ns/event batched
- **[`moveable_signal`](#moveable_signal)** — thread-safe signal/slot whose connections survive moves, with no lock held while slots run
- **[working demos](#demos)** — event capture and bit-exact replay over multi-hop topologies, real pcap decode and replay, Taskflow-style dependency graphs

One theme unifies all of it: **simplicity, one rule, nominal overhead**. The rule: every type keeps the **integrity of its state** across a move — a move happens on a quiescent object or fails loudly — which hands classes composed from these types the [rule of zero](https://en.cppreference.com/cpp/language/rule_of_three) back: write no special member functions, and the compiler generates correct moves. The overhead: composition wrappers are the same size as what they wrap (the tests `static_assert` it), the safety checks are a `try_lock` probe or a relaxed flag, and everything is header-only, C++17 and later, dependency-free, `cassert`-tested, and ThreadSanitizer-verified across the CI matrix.

## Quick start

Header-only, zero dependencies, C++17 or later — **nothing to build**. Drop the headers on your include path (or use CMake [FetchContent / `find_package`](#cmake)), then include the umbrella — or just the one header you need:

```cpp
#include "ts_moveables.hpp"        // everything, or e.g. #include "moveable_mutex.hpp"
```

The whole point, in five lines — a concurrent class that just *moves*:

```cpp
struct Account {
    snicholls::moveable_mutex<>           m;          // a plain std::mutex here would delete Account's move
    snicholls::moveable_atomic<long long> balance{0};
};
std::vector<Account> accounts;                         // grows and reallocates fine — moves are compiler-generated
```

No special member functions to write; the rule of zero is back. **Now reach for:**

| You want to… | Use |
|---|---|
| make a class holding a mutex / atomic / cv / … **moveable** | the [moveable primitive](#the-types) — `moveable_mutex`, `moveable_atomic`, `moveable_semaphore`, `moveable_latch`, … |
| touch a value **only under its lock** | [`synchronized<T>`](#synchronizedt) (+ heterogeneous `variant` / `tuple` / `any` / type-map / bag) |
| **hand off between two threads**, lock-free | [`circular_buffer<T>`](#circular_buffer) — reach for `push_n`/`pop_n` when you need throughput |
| **broadcast an event** to many typed listeners | [`moveable_signal`](#moveable_signal) |
| a **pipeline** with consumer dependency graphs | [`disruptor<T>`](#disruptor) |
| **run tasks on a pool** | [`task_pool`](#thread_pool) — `work_stealing_` for fork-join, `mpmc_` for general submit, `dispatch_` for a single feed |
| the **fastest possible raw single-op queue** | honestly? [moodycamel](#which-should-you-use). We tell you when *not* to pick us. |

Build and test locally: `make test` (or `cmake -B build && ctest --test-dir build --output-on-failure`). Pick the standard with `make test STD=c++17`. That's it.

## Why this library?

One line of code silently deletes your class's move and copy operations:

```cpp
struct Account {
    std::mutex m;               // Account is now immovable
    long long  balance{0};
};

std::vector<Account> accounts;
accounts.emplace_back();        // error: somewhere deep inside <vector>
```

Everyone who writes concurrent classes hits this, and the standard workarounds all cost something:

| Workaround | Cost |
|---|---|
| `std::unique_ptr<std::mutex>` | a heap allocation per object, a pointer chase on every lock, weaker cache locality, a null state to reason about |
| Hand-written move constructor and assignment | the rule of zero is lost: per-class boilerplate that must be revisited every time a member changes, and the concurrency is easy to get wrong |
| Bend the design — `std::deque`, `reserve()` up front, external lock tables, indices instead of objects | your data structures are dictated by a member's quirk |

What this library restores is the **[rule of zero](https://en.cppreference.com/cpp/language/rule_of_three)**. Compose your class from these types and the compiler generates correct move (and, where meaningful, copy) operations. The decision of what a move *means* for each primitive — value copied, permits transferred, "nobody may be waiting" — is made once, here, instead of ad hoc in every class that owns a mutex.

Beyond that, you get three things the workarounds do not give you:

- **No overhead where it matters.** The composition wrappers have the same size and layout as the primitive they wrap — `sizeof(moveable_mutex<>) == sizeof(std::mutex)` — with no allocation and no indirection; the unit tests `static_assert` this. (`moveable_condition_variable` pays one `int` for waiter tracking; the reimplemented types are small mutex + condition-variable classes.)
- **Defined behaviour where the language gives you none.** Destroying or replacing a mutex that another thread holds is undefined behaviour. Here, moving an in-use primitive throws `std::runtime_error` — misuse becomes a deterministic, debuggable exception instead of silent corruption.
- **Portability.** The semaphore, latch and barrier work on C++17, three years before their std equivalents.

To be honest about the limits: no library can make it meaningful to move an object *while other threads are using it* — that is a design problem, not a library problem. Moves belong to the quiescent phases of an object's life: building up containers, returning from factories, handing off between pipeline stages. This library makes those phases ergonomic, and makes everything else fail loudly.

## The contract

Every type here follows the same rule: **a move either happens on a quiescent object or it does not happen at all.**

- Types whose state is a plain value (atomics, once flags) copy/move that value atomically.
- Types whose state is "who is blocked on me" (mutexes, condition variables, spin locks) verify nobody holds or waits on them, and otherwise throw `std::runtime_error` rather than silently corrupting a lock another thread relies on.
- Types whose state is a protected count (semaphores, latches, barriers) transfer that count under their own lock, and throw `std::runtime_error` if any thread is blocked on either side of the move.

Moved-from objects are always left valid and usable.

## The types

| Header | Type | Wraps / replaces | Copy | Move |
|---|---|---|---|---|
| `moveable_atomic.hpp` | `moveable_atomic<T>` (+ aliases such as `moveable_atomic_int`) | `std::atomic<T>` | value | value |
| `moveable_atomic.hpp` | `moveable_atomic_flag` | `std::atomic_flag` (built on `std::atomic<bool>`) | value | value |
| `moveable_mutex.hpp` | `moveable_mutex<M>` (+ aliases for recursive / timed / shared / shared timed) | any std mutex | — | checked |
| `moveable_spin_lock.hpp` | `moveable_spin_lock` | the classic `std::atomic_flag` spin lock | — | checked |
| `moveable_condition_variable.hpp` | `moveable_condition_variable<CV>` / `moveable_condition_variable_any` | `std::condition_variable(_any)` | — | checked |
| `moveable_once_flag.hpp` | `moveable_once_flag` + `snicholls::call_once` | `std::once_flag` / `std::call_once` | state | state, checked |
| `moveable_semaphore.hpp` | `moveable_semaphore` | `std::counting_semaphore` | — | count transfers |
| `moveable_latch.hpp` | `moveable_latch` | `std::latch` | — | count transfers |
| `moveable_barrier.hpp` | `moveable_barrier<Completion>` | `std::barrier` | — | config + phase transfer |
| `synchronized.hpp` | `synchronized<T, M>` / `synchronized_waitable<T, M>` | `folly::Synchronized` / P0290 | locked | locked / checked |
| `synchronized_heterogeneous.hpp` | `synchronized_variant<Ts...>`, `synchronized_tuple<Ts...>`, `synchronized_any`, `synchronized_type_map`, `synchronized_bag` | — | locked | locked |
| `circular_buffer.hpp` | `circular_buffer<T>` / `circular_buffer<T, N>` | `boost::lockfree::spsc_queue` (immovable) | checked | checked, contents transfer |
| `mpmc_queue.hpp` | `mpmc_queue<T>` (bounded, lock-free) | Vyukov bounded MPMC / moodycamel | — | quiescent, contents transfer |
| `work_stealing_deque.hpp` | `work_stealing_deque<T>` (bounded Chase-Lev) | Chase-Lev / Taskflow internals | — | — (internal, stable) |
| `disruptor.hpp` | `disruptor<T, WaitStrategy>` | the LMAX Disruptor pattern | — | handle transfer, always safe |
| `moveable_signal.hpp` | `moveable_signal<Args...>` + `connection` / `scoped_connection` | Boost.Signals2 / sigslot | — | connections survive the move |
| `thread_pool.hpp` | `task_pool` interface + `mutex_` / `sharded_` / `dispatch_` / `mpmc_` / `work_stealing_task_pool` | Taskflow / TBB / `std::async` | — | moveable handle (heap core) |
| `ts_moveables.hpp` | umbrella header — includes everything | | | |

Two implementation strategies are used:

1. **Composition** (`moveable_atomic`, `moveable_mutex`, `moveable_condition_variable`): the std type is held by value and its full API is forwarded (`lock`, `try_lock_for`, `lock_shared`, `wait_until`, …). Forwarding members are only instantiated when called, so `moveable_mutex<std::mutex>` compiles even though `std::mutex` has no `lock_shared`. A `native()` accessor exposes the wrapped std object.
2. **Portable reimplementation** (`moveable_semaphore`, `moveable_latch`, `moveable_barrier`, `moveable_once_flag`, `moveable_spin_lock`): the std types are immovable *and* their state is unobservable, so a wrapper could never preserve it. These are small mutex + condition-variable (or single-atomic) implementations whose state is fully protected, which makes the move guarantee exact rather than best-effort. They also work on C++17, where `<semaphore>`, `<latch>` and `<barrier>` do not exist.

The lockable types satisfy the standard *BasicLockable* / *Lockable* / *SharedLockable* requirements, so they compose with `std::lock_guard`, `std::unique_lock`, `std::shared_lock`, `std::scoped_lock` and `std::condition_variable_any`.

## Example

```cpp
#include "ts_moveables.hpp"

struct Account
{
    snicholls::moveable_mutex<>              m;
    snicholls::moveable_condition_variable<> funds_arrived;
    snicholls::moveable_once_flag            opened;
    snicholls::moveable_atomic_uint64_t      deposits{0};
    long long                                balance{0};

    void deposit(long long amount)
    {
        opened.call_once([]{ /* open the account */ });
        {
            std::lock_guard<snicholls::moveable_mutex<>> lock(m);
            balance += amount;
            ++deposits;
        }
        funds_arrived.notify_all();
    }
};

// The compiler generates the move operations - and Account can now
// live in a vector, which was the whole point:
std::vector<Account> accounts;
accounts.emplace_back();        // reallocation moves Accounts safely
```

Moving while another thread holds a lock or waits refuses loudly instead of corrupting state:

```cpp
snicholls::moveable_mutex<> m;
// ... another thread calls m.lock() ...
auto stolen = std::move(m);     // throws std::runtime_error
```

## synchronized&lt;T&gt;

Built on the primitives: a value bonded to its mutex, where the *only* way to reach the value is a closure that runs with the lock held. Closures that try to return a reference are rejected at compile time — the lock cannot be escaped.

```cpp
snicholls::synchronized<std::vector<int>> items;

items.with_lock([](auto& v) { v.push_back(42); });
auto n = items.with_lock([](const auto& v) { return v.size(); });
```

`M` may be any BasicLockable: `std::shared_mutex` enables `with_read_lock`, `std::recursive_mutex` allows nesting, `moveable_spin_lock` suits very short sections. Copy and move take the source's lock, and the mutex itself never moves — so `synchronized` needs no quiescent contract at all; its copy and move are themselves thread-safe with respect to the source. `synchronized_waitable<T>` adds the producer/consumer vocabulary — `update()` (mutate then wake all waiters), `wait(pred)`, `wait_then(pred, consume)` — with waiter tracking, so moving while threads are blocked throws, exactly like the raw condition variable.

It is also the answer to "a thread-safe heterogeneous container": compose it with the standard heterogeneous types rather than inventing a new container. `synchronized_heterogeneous.hpp` ships the compositions ready made, each with the typed conveniences that make it pleasant:

```cpp
snicholls::synchronized_variant<Idle, Running, Done> state;     // closed set
state = Running{};
state.visit([](const auto& s) { /* runs under the lock */ });

snicholls::synchronized_tuple<Config, Stats> bundle;            // fixed bundle
bundle.set<Config>(Config{5});
auto stats = bundle.get<Stats>();

snicholls::synchronized_type_map ctx;                           // one value per type
ctx.put(Config{3});
ctx.with<Config>([](Config& c) { ++c.retries; });               // race-free read-modify-write

snicholls::synchronized_bag bag;                                // open, ordered bag
bag.push(42);
bag.push(std::string("x"));
auto ints = bag.extract<int>();
```

(plus `synchronized_any` for a single value of unknown type). Value-returning reads (`try_get`, `holds`, `count`) are snapshots; the closure forms (`visit`, `apply`, `with`, `for_each`) are the race-free read-modify-write paths. Everything is built on `synchronized<T>`, so the whole structure still moves.

## circular_buffer

A single-producer / single-consumer ring whose entire concurrent state is two indices you can point at — `tail` (producer) and `head` (consumer), each an atomic on its own cache line, each side keeping a private cached copy of the other's index so the common push/pop touches no shared cache line at all. Capacity is a power of two; indices increase monotonically and wrap by mask, so full and empty are distinguished by the difference and no slot is wasted. Every memory-ordering choice is commented in the header with the reason it is what it is.

```cpp
snicholls::circular_buffer<Message> ring{1024};     // runtime capacity, one allocation ever
snicholls::circular_buffer<Message, 1024> fixed;    // compile-time capacity, zero allocation

// producer thread                     // consumer thread
ring.try_push(msg);                    auto m = ring.try_pop();          // optional<Message>
ring.push_n(first, n);                 ring.pop_n(out_iterator, max);    // batched - amortises the atomic traffic
```

`try_push`/`try_pop` are wait-free. `size()`/`empty()`/`full()` are safe from any thread, as snapshots. Moves transfer the queued elements in order and leave the source empty but fully usable, capacity intact; copies are snapshots.

The quiescent move-check costs the hot path **nothing**. A lock-free ring has no lock to probe, so rather than flag every op, move/copy samples `head_`/`tail_` over a short window and throws `std::runtime_error` if either index advances — a live push or pop moves an index and is caught, while push/pop themselves maintain no activity flag and pay nothing. It is best-effort by design (it makes concurrent misuse loud, not impossible), and there is no safety-vs-speed knob to choose: one ring type, always checked, always free on the hot path.

## disruptor

Phase 1 of the [LMAX Disruptor](https://lmax-exchange.github.io/disruptor/) pattern: a pre-allocated ring of events, sequence counters instead of queue locks, consumers that see contiguous batches, and explicit dependency graphs. Single producer; each consumer is pumped by one thread via `poll()` or `run()`. Wait strategies (busy-spin / yielding / blocking) plug in per disruptor.

```cpp
snicholls::disruptor<Trade> d{4096};
auto& journal = d.add_consumer();               // parallel stage
auto& enrich  = d.add_consumer();               // parallel stage
auto& publish = d.add_consumer({&journal, &enrich});  // sees events only after both

// producer thread                       // consumer threads
d.publish([&](Trade& t) {                journal.run(keep_going, [](Trade& t, std::int64_t seq, bool end_of_batch) {
    t = incoming;                            // handlers get batches, in order
});                                      });
```

The producer claims, mutates in place, and publishes with one release store — no allocation after construction, gated so it can never lap the slowest consumer. A design note on moveability: all shared state lives behind a stable heap core, so the disruptor *handle* moves freely even while producer and consumers are running — consumer references stay valid — at the price of one pointer indirection on the hot path.

## moveable_signal

Thread-safe signal/slot for any object. Everything — connect, disconnect, emit, slot execution — may happen from any thread, and the classic failure modes are designed out:

- **Emission never holds the signal's lock while calling user code.** An emit grabs an immutable snapshot of the slot list (one brief lock, a `shared_ptr` copy) and invokes without it — slots may freely connect, disconnect or re-emit, deadlock-free by construction, and the emit path allocates nothing.
- **Slots run in connection order** (several popular libraries do not promise this).
- **Lifetime tracking**: `connect(shared_ptr, &Object::method)` auto-disconnects when the object dies, and holds the object alive for the duration of any call already dispatched to it.
- **Connections survive moves** — the differentiator none of the incumbents offer: connections bind to the signal's shared internal state, never to the signal object's address, so a signal member moves freely with its owner.

```cpp
struct Button {
    snicholls::moveable_signal<int> clicked;    // Button still gets the rule of zero
};

Button b;
auto c = b.clicked.connect([](int n) { /* ... */ });
snicholls::scoped_connection sc(b.clicked.connect(obj, &Handler::on_click));

std::vector<Button> panel;
panel.push_back(std::move(b));                  // move it anywhere -
panel[0].clicked(1);                            // every connection still fires
```

(The name is `moveable_signal` rather than `signal` because POSIX declares a global C function `::signal`, which makes the unqualified short name ambiguous the moment `<csignal>` leaks into a translation unit.)

The architecture this enables at scale — typed emitters, observers, meshes, capture and replay — is shown working in the [Demos](#demos) section below.

## Building and testing

The library is header-only — just add the headers to your include path.

The unit tests are deliberately lightweight: plain `cassert`, no framework.

```sh
make test    # build and run the unit tests
make tsan    # the same tests under ThreadSanitizer
make asan    # the same tests under Address + UB Sanitizers
make demo    # build and run the demo (same code as the Xcode target)
make bench   # dependency-free throughput benchmarks (ring vs mutex/spin/cv baselines)
make bench-compare   # head-to-head vs moodycamel (fetches its headers on demand; not a dependency)

make test STD=c++17   # any target can be built against a different standard
```

The Xcode project builds the same demo (`main.cpp`).

### CMake

The library exports the interface target `snicholls::ts_moveables`, which carries the C++17 requirement, Threads, and (on Linux) libatomic. The easiest integration is FetchContent:

```cmake
include(FetchContent)
FetchContent_Declare(ts_moveables
    GIT_REPOSITORY https://github.com/saxonnicholls/ts-moveables.git
    GIT_TAG main)
FetchContent_MakeAvailable(ts_moveables)

target_link_libraries(my_app PRIVATE snicholls::ts_moveables)
```

`add_subdirectory` works the same way. Or build, test and install it system-wide:

```sh
cmake -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake
ctest --test-dir build-cmake --output-on-failure   # the same cassert unit tests
cmake --install build-cmake                        # then: find_package(ts_moveables REQUIRED)
```

However the library arrives — vendored, fetched or installed — `#include "ts_moveables.hpp"` is the same.

CI runs the test suite on every push across Linux (x86-64 and ARM64, GCC and Clang), macOS (Apple Silicon and Intel, Apple Clang), and Windows (MSVC), in both C++17 and C++20, with ThreadSanitizer on every POSIX platform and Address + UB Sanitizers on Linux.

### About ThreadSanitizer

A library whose entire promise is concurrent state integrity should be held to more than "the tests pass". ThreadSanitizer verifies the synchronization this library *claims*, not just the answers it produces: TSan models C++ atomics and their memory orderings precisely, so the acquire/release pairing in the spin lock and once flag, the atomic waiter counts in the condition variable, and the lock-protected count transfers in the semaphore, latch and barrier moves are all checked against the C++ memory model as the tests run. A race between a move and a concurrent lock, wait or arrival is exactly the class of bug TSan exists to catch.

Four practical notes:

- TSan is a dynamic tool — it reports races in executions that *happen*, not in all executions that *could*. The tests are written to give it something to bite on: contended counters across eight threads, waiters genuinely blocked while a move is attempted, repeated barrier phases. A clean TSan run is strong evidence, not a proof.
- Expect a 5–15× slowdown, and do not combine it with AddressSanitizer in the same binary — `make asan` is a separate target for exactly that reason.
- `make tsan` loads one suppression file, `tests/tsan.supp`: libstdc++'s timed mutexes take their locks through `pthread_mutex_clocklock`, which some libtsan builds (GCC's on current Ubuntu, for instance) do not intercept, so TSan never sees the lock and misreports the matched unlock as "unlock of an unlocked mutex". The suppression is scoped to the std timed mutex unlock frames only — genuine mutex misuse still reports. (Newer libstdc++ disables that path under TSan but leaves the `steady_clock` `try_lock_until` overload calling it — GCC bug 113327 — so under TSan the tests use `system_clock` deadlines for the timed mutexes.)
- If `make tsan` dies immediately with an illegal instruction before printing anything, the toolchain's TSan runtime is broken, not the tests — we have seen this on some Xcode/Intel macOS combinations. The CI matrix is the reliable reference.

To check your own code that composes these types, the same single flag applies: `-fsanitize=thread` with Clang or GCC, or enable "Thread Sanitizer" in your Xcode scheme's diagnostics.

## Throughput

Measured on the reference machine — a 2014 Intel iMac, Apple Clang, `-O3` — via `make bench` and `make demo-signals`. Treat them as *relative* guidance and rerun on your own hardware. CI runs both harnesses on every push and publishes these same tables to each run's **Summary page**, per platform (informational — never a pass/fail gate; both harnesses accept `--markdown`). Single-op SPSC numbers swing severalfold with thread placement (sibling hyperthreads share cache; separate cores bounce it) — the batched numbers are stable precisely because batching amortises that traffic.

> **On Apple Silicon** (macOS ARM64, measured by CI) the numbers reach genuinely elite territory:
> - **batched disruptor: 1827 Mops/s — 0.5 ns/event**, sub-nanosecond per event
> - **batched ring: 965 Mops/s — 1.0 ns/op**
> - **signal mesh: 128 GB/s**, ~300 billion events/hour sustained in constant memory
> - **work-stealing fork-join: 14.7 M tasks/s**, double the mutex pool (7.4 M) — the locality win, on hardware with a weaker memory model that stresses the lock-free orderings hardest
>
> The reference-machine tables below are the older Intel box; every platform's live numbers are on the CI run's Summary page.

`make bench` — SPSC, 2M items, one producer and one consumer thread, best of 5:

| Case | Throughput | Per op |
|---|---|---|
| `circular_buffer` singles | 30–260 Mops/s (placement-sensitive) | 4–33 ns |
| `circular_buffer` batched (64) | ~420–480 Mops/s | ~2.3 ns |
| `disruptor` publish/poll | ~25–29 Mops/s | ~38 ns |
| **`disruptor` publish_n/poll (64)** | **~860 Mops/s** | **~1.2 ns** |
| `std::mutex` + `std::queue` | ~9 Mops/s | ~110 ns |
| `moveable_spin_lock` + `std::queue` | ~14 Mops/s | ~70 ns |
| `synchronized_waitable<std::queue>` (cv) | ~6 Mops/s | ~160 ns |

Thread pools (`make bench` also, 500k trivial tasks, submit + `wait_idle`):

| Pool | Throughput | Per task |
|---|---|---|
| **`dispatch_task_pool`** (lock-free ring, single-dispatcher) | **~6–7 M tasks/s** | **~150 ns** |
| `mpmc_task_pool` (lock-free MPMC, submit-anywhere) | ~3.6 M tasks/s | ~280 ns |
| `work_stealing_task_pool` (external submit → injector) | ~1.7 M tasks/s | ~570 ns |
| `mutex_task_pool` (one cv-blocked queue) | ~0.5 M tasks/s | ~1900 ns |
| `sharded_task_pool` (K cv-blocked queues) | ~0.4 M tasks/s | ~2200 ns |
| `std::async` (thread per task) | ~0.07 M tasks/s | ~15000 ns |

And fork-join, where tasks spawn sub-tasks from inside a worker — work-stealing's home turf:

| Pool | Throughput | Per task |
|---|---|---|
| **`work_stealing_task_pool`** (fork-join) | **~7.5 M tasks/s** | **~130 ns** |
| `mutex_task_pool` (fork-join) | ~3.4 M tasks/s | ~300 ns |

Two honest shapes here. First, the cv-backed pools are wakeup-latency-bound (~2 µs per task is condition-variable signalling, not the queue); the lock-free pools avoid that and run several times faster. Second, `work_stealing_task_pool` looks mid-pack on *external* trivial submits (every task funnels through the injector and pays a heap allocation) but is the fastest of all on **fork-join** — 4× its own external number — because spawned work lands in hot local deques instead of a shared queue. That is exactly why work-stealing exists, and why the comparison harness matters: no single pool wins every shape.

### Head-to-head vs moodycamel

To ground the claims honestly, `make bench-compare` runs the *same* harness against [moodycamel](https://github.com/cameron314/concurrentqueue)'s lock-free queues — the field's reference implementations. The headers are fetched on demand (`scripts/fetch_bench_deps.sh`, gitignored); they are **not** a library dependency. On the Intel reference machine:

| Case | Implementation | Throughput (this Intel box, unpinned) |
|---|---|---|
| SPSC | `snicholls::circular_buffer` | ~45 Mops/s typical (spikes to ~300–480 when the two threads share cache) |
| SPSC | `moodycamel::ReaderWriterQueue` | **~430 Mops/s** (placement-robust) |
| MPMC 4×4 | `snicholls::mpmc_queue` (bounded) | ~3.5 Mops/s (stable) |
| MPMC 4×4 | `moodycamel::ConcurrentQueue` (unbounded) | **~6.5 Mops/s** (stable) |

**moodycamel wins — ~9× on SPSC single-op, ~1.9× on MPMC — and that is the honest answer to "do we beat the specialists on raw single-op throughput": no.** The gap is architectural, and understanding it is worth more than the number:

- **moodycamel's queues are block-based**: they touch their shared atomics roughly *once per block* (~hundreds of elements). Our ring is slot-based: it touches a shared atomic *every op*. So moodycamel's coherence traffic is ~1/512 of ours, which makes it both faster and placement-robust. Our ring's throughput swings with thread placement (~45 Mops/s on separate cores, spiking to ~300–480 when the OS happens to land producer and consumer on cache-sharing sibling threads) — moodycamel holds its number regardless. Closing that gap would mean *becoming* moodycamel — a fundamentally more complex queue — the opposite of this ring's teaching-quality design.
- **The move-check is free, so there is no safety/performance tradeoff to expose.** It samples the indices at move time rather than flagging every op, so push/pop pay nothing for it (this is what "one ring type, always safe" buys — no knob, and ~27 % faster than the earlier flag-based ring as a bonus). The remaining gap is entirely the block-vs-slot difference above, not the check.
- **Where batching applies, we close the gap on our own terms.** `circular_buffer::push_n` and the disruptor's `publish_n` hit 400–1800 Mops/s (tables above) because batching amortises the atomic traffic the same way moodycamel's blocks do — and *stably*, no placement drama, since each atomic op now covers 64 elements. On Apple Silicon the batched disruptor's 0.5 ns/event is squarely in moodycamel's single-op league. (`mpmc_queue`'s stable ~1.9× gap to `ConcurrentQueue` is the same story — per-cell CAS vs block-based.)

So the positioning the [non-goals](FUTURE_DIRECTIONS.md) always stated holds: we do **not** compete with the specialist lock-free queues on raw single-op throughput, and don't claim to. We offer what they don't — moveability, the quiescent contract, a bounded option, and a toolkit that composes — at throughput that is more than enough for the overwhelming majority of concurrent code, and genuinely fast in batch. Run `make bench-compare` yourself; the single-op numbers move run to run with placement, so understanding *why* beats any single row.

### Which should you use?

A fair decision guide — this is not a "we're best" pitch:

- **Reach for [moodycamel](https://github.com/cameron314/concurrentqueue) (or Boost.Lockfree, or TBB)** when a *single queue on a hot path* is your measured bottleneck and you need tens-to-hundreds of millions of single-item enqueue/dequeue per second. They are specialist, battle-hardened, and faster than us there by design (~9× SPSC, ~1.9× MPMC). If that is your problem, use them — our own non-goals say so.
- **Reach for this library's `circular_buffer` / `mpmc_queue`** when you want any of: the queue to be a **member of a moveable object** (moodycamel's are immovable — this is the gap nobody else fills); a **bounded, header-only, dependency-free, C++17** queue with a loud misuse check that costs the hot path nothing; or one coherent, composable toolkit alongside the disruptor, pools, and signal. Their single-op throughput (~tens of millions/s) is not the bottleneck in almost any real system.
- **Reach for the batch APIs** — `push_n`/`pop_n`, the disruptor's `publish_n` — when you *do* need high throughput from this library. Batching amortises the per-op atomic and lands at 400–1800 Mops/s, competitive with moodycamel and placement-stable. This is the right tool for streaming and market-data fan-out.

**Realistic use cases for our queues:** inter-thread messaging, event pipelines, task dispatch, market-data fan-out, journal/replay — anywhere millions of items/second is plenty (i.e. nearly everywhere), and where moveability, boundedness, or the toolkit matter. **Not a realistic expectation:** beating the specialist queues on a raw single-op microbenchmark. We lose that by ~9×, on purpose, and no flag or trick in this library changes it — if that microbenchmark is genuinely your production bottleneck, use the specialist. The library's value is *moveable concurrent objects and a toolkit that composes*, delivered at throughput that suffices for the vast majority of real code — not a new lock-free speed record.

`make demo-signals` — the typed signal/slot architecture, 5M emissions per scenario:

| Scenario | Rate | Bandwidth |
|---|---|---|
| 1 emitter → 1 observer | ~32 M events/s | |
| 1 emitter → 8 observers | ~163 M deliveries/s | |
| multi-type plant (Tick/Trade/Heartbeat) | ~41 M deliveries/s | |
| 4 threads emitting into 1 observer | ~9 M events/s | |
| many-to-many mesh 4×4 | ~12 M deliveries/s | |
| 4 KiB frames → 1 observer, checksummed | ~12 M frames/s | ~48 GB/s |
| many-to-many mesh 4×4, 4 KiB frames | ~15 M deliveries/s | ~64 GB/s |

Sustained across the whole signal demo: ~27M deliveries/s — on the order of **95 billion events per hour** — in constant memory. The GB/s figures are bytes actually checksummed by consumers; payloads move zero-copy by `const&`, which is why delivery bandwidth can exceed DRAM bandwidth. The batched disruptor headline is the LMAX thesis in one row: amortise the coordination and the cost per event approaches a nanosecond.

The self-verifying demos (every number below was produced while asserting ordering and bit-exact replay hashes):

| `make demo-capture` — 3-hop capture/replay topology | Result |
|---|---|
| pipeline, 3 reentrant hops, no capture | ~10 M events/s (~35 billion events/hour) |
| ingress journal overhead | **~10–13 ns/event** |
| replay from journal (fresh topology, hashes identical) | ~10 M events/s |
| 4 partitioned pipelines on 4 threads | ~11–14 M events/s total |

| `make demo-pcap` — packet decode + flow partition | Result |
|---|---|
| live: decode → 4 flow nodes, every payload byte hashed | ~6.8 M packets/s, ~0.6 GB/s |
| replay from the offset journal (hashes identical) | ~6.9 M packets/s |

| `make demo-taskflow` — dependency graphs on signals | Result |
|---|---|
| fan-out/fan-in 1→64→1, join counter as the scheduler | ~23 M task executions/s |
| 4 independent diamond graphs on 4 threads | ~15 M waves/s |
| 4-stage pipeline, 4 concurrent producers | ~1.7 M tokens/s end to end |
| computation-as-event (submit → compute → result) | ~16 M jobs/s round-tripped |

## Demos

Working programs, not snippets. Each builds and runs with one make target; the last three verify their own correctness and fail loudly, so CI runs them on every push as cross-platform integration tests.

| Demo | Run with | What it shows |
|---|---|---|
| [signal_slot_demo](demos/signal_slot_demo.cpp) | `make demo-signals` | the typed emitter/observer architecture: per-type emitters, virtual observers with RAII subscriptions, multi-type plants and many-to-many 4×4 meshes wired with fold expressions — measured in events/s and checksummed GB/s |
| [capture_replay_demo](demos/capture_replay_demo.cpp) | `make demo-capture` | the HFT discipline: journal every ingress event (~13 ns/event), replay through a fresh multi-hop topology, and prove the egress stream hash reproduces exactly — ordering asserted at every hop, live and replayed, single-threaded and across four partitioned pipelines |
| [pcap_replay_demo](demos/pcap_replay_demo.cpp) | `make demo-pcap` | the same discipline over real network captures: a dependency-free classic-pcap reader, packets travelling zero-copy by `const&` through decode and flow-partition nodes, a journal of 4-byte offsets, and a bit-exact replay |
| [taskflow_style_demo](demos/taskflow_style_demo.cpp) | `make demo-taskflow` | [Taskflow](https://taskflow.github.io)-style dependency graphs on signals: diamonds, 1→64→1 fan-in joins, graph reuse, concurrent pipelines and computation-as-event — with no scheduler, so no thread starvation: a task runs inline on the thread that completes its last dependency |

For the pcap demo, bring your own data — `./build/pcap_replay_demo capture.pcap` — or capture live traffic with [scripts/capture_pcap.sh](scripts/capture_pcap.sh), which auto-detects your default interface (`en0` on macOS, `eth0`-style on Linux), runs `sudo tcpdump -s 0 -w`, and prints the replay command. Public capture files to experiment with are indexed at [netresec.com/?page=PcapFiles](https://www.netresec.com/?page=PcapFiles) — note that many are pcapng or gzipped, and the reader takes classic pcap, so convert first: `tcpdump -r in.pcapng -w out.pcap`. Without any file, the demo synthesises a capture, so it always runs.

## thread_pool

A pure-virtual `task_pool` interface — the point is *comparison*: good thread pools are rare, and the design choices (one shared queue vs. sharded vs. lock-free hand-off; block vs. spin) are hard to weigh without a common surface. Three implementations, each built visibly from this library's own primitives, span the spectrum:

```cpp
snicholls::mutex_task_pool         pool{4};   // one synchronized_waitable queue — the honest baseline
snicholls::sharded_task_pool       pool{4};   // K queues, round-robin — one hot lock becomes K cool ones
snicholls::dispatch_task_pool      pool{4};   // one lock-free circular_buffer per worker (single-dispatcher)
snicholls::mpmc_task_pool          pool{4};   // one shared lock-free MPMC queue — general submit-anywhere
snicholls::work_stealing_task_pool pool{4};   // per-worker Chase-Lev deques + stealing — best for fork-join

pool.submit([] { /* work */ });
auto fut = snicholls::async(pool, [] { return 42; });   // result-returning, over the interface
pool.wait_idle();
```

Each is built visibly from the library's own parts. `dispatch_task_pool` is where the SPSC `circular_buffer` fits — its **single-dispatcher** contract (submit from one thread) is the deterministic feed-handler pattern, named honestly rather than hidden behind a submission lock. `mpmc_task_pool` is the general submit-anywhere pool, built on the lock-free [`mpmc_queue`](#the-types). `work_stealing_task_pool` is the design that competes with the incumbents on load balance: each worker owns a Chase-Lev deque (a task that spawns work pushes to its own hot deque), a shared `mpmc_queue` absorbs external submissions, and idle workers steal from random victims — with an inline-on-saturation policy so a fork-join can never deadlock. Pools are moveable (heap core, disruptor-style); polymorphic users hold them through `std::unique_ptr<task_pool>`.

We do **not** claim to beat the work-stealing greats (Taskflow, TBB, Tokio) — they have years of hardening. What we add is narrow and real: pools composed from the toolkit, moveable handles, a bounded lock-free MPMC queue and a Chase-Lev deque as reusable components, dependency-free, and a level comparison harness. It also completes the [taskflow demo](#demos)'s story — *static* graphs need no pool; *dynamic* submission is where a pool earns its place.

The two lock-free building blocks stand alone too: `mpmc_queue<T>` is a bounded Vyukov MPMC ring (moveable when quiescent), and `work_stealing_deque<T>` is a bounded Chase-Lev deque with the memory-model-verified orderings from Le et al. (2013).

The roadmap and the reasoning behind every component — including the non-goals and what was deliberately *not* built — live in [FUTURE_DIRECTIONS.md](FUTURE_DIRECTIONS.md). Most of it has now shipped; what remains is disruptor phase 2 (multi-producer) and a signal/slot refinement.

## The bigger picture: the host side of accelerated systems

The heavy parallel math increasingly runs on accelerators — NVIDIA GPUs (CUDA), FPGAs, and tensor engines / TPUs. But every one of them sits behind a **CPU host** that has to feed it, drain it, orchestrate it, and hold the state around it — and in real pipelines that host-side coordination, not the device, is often the limiter. Keeping the accelerator busy is a CPU concurrency problem.

This library is that host side. It does not run on the device and does not try to — it is the dependency-free CPU fabric that surrounds one, and it complements the accelerator toolchains (CUDA/cuDNN/XLA, FPGA HLS, vendor runtimes) rather than competing with them:

- **Staging and hand-off.** `circular_buffer`, `mpmc_queue`, and the `disruptor` are the host-side producer/consumer rings that batch work *toward* a device — H2D copies, kernel and stream enqueues, DMA descriptors — and drain results back. Their batch APIs (`push_n`, `publish_n`) line up exactly with how you feed an accelerator: amortise the transfers, launch in batches. Batch throughput is our strength, and batch is precisely what devices want.
- **Orchestration and lifecycle.** The moveable primitives, `synchronized<T>`, and the thread pools build the host runtime objects that own CUDA streams, device buffers, FPGA DMA channels and handles — and *move cleanly*. A class holding a stream or a device pointer gets the rule of zero back, which is exactly the ownership hygiene a heterogeneous runtime needs.
- **Ingest and routing.** `moveable_signal` and the capture/replay demos are the event fabric that gets data to the device and results to whoever consumes them, deterministically and replayably — the same discipline an HFT feed handler or an ML data loader needs before the tensor cores ever spin up.

So the relationship is complementary: the GPU/FPGA/TPU does the compute; this is the low-overhead, moveable CPU coordination layer that keeps it fed. Where the host is the bottleneck — feeding the beast — a clean, dependency-free fabric is exactly what you want, and it stays out of the way of whatever device toolchain you pair it with.

## Caveats

- The quiescence probe on `moveable_mutex` and `moveable_spin_lock` is a `try_lock`; on a recursive mutex it cannot detect a lock held by the moving thread itself. Moving a mutex you yourself hold is still a logic error.
- A mutex/condition-variable move is safe against *stale* use, not against *concurrent* use: a thread may lock or wait immediately after the probe. Move objects when they are quiescent; the checks exist to make misuse loud.
- `moveable_barrier`'s completion function runs while the barrier's internal lock is held; it must not call back into the barrier (as with `std::barrier`, where that is undefined behaviour).

## Contributing

Pull requests are **more than welcome** — I'm happy to collaborate. Bug reports, new moveable types, portability and compiler fixes, benchmark numbers from your own hardware, sharper tests, or just a good question all move this forward. Open an issue or a PR and let's build it together.
