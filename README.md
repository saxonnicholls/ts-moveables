# ts-moveables
Thread Safe Moveable Objects

[![CI](https://github.com/saxonnicholls/ts-moveables/actions/workflows/ci.yml/badge.svg)](https://github.com/saxonnicholls/ts-moveables/actions/workflows/ci.yml)

We often need to move, so called "immovable" objects in C++ such as atomics, mutexes and condition variables. This code provides portable mechanisms to do this. It is simple and proven: header-only, C++17 and later, no dependencies, tested with plain `cassert` unit tests.

Of course these types are not *truly* moveable — the point is to **keep the integrity of the state** across a move so that ordinary objects composed from them can be written safely, with compiler-generated move (and where sensible, copy) operations.

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
| `disruptor.hpp` | `disruptor<T, WaitStrategy>` | the LMAX Disruptor pattern | — | handle transfer, always safe |
| `moveable_signal.hpp` | `moveable_signal<Args...>` + `connection` / `scoped_connection` | Boost.Signals2 / sigslot | — | connections survive the move |
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

`try_push`/`try_pop` are wait-free. `size()`/`empty()`/`full()` are safe from any thread, as snapshots. Moves transfer the queued elements in order and leave the source empty but fully usable, capacity intact; copies are snapshots. The quiescent check here is honest best-effort — a lock-free ring has no lock to probe, so each side raises a flag (two relaxed stores to a cache line it already owns, effectively free) and move/copy throws `std::runtime_error` if a push or pop is literally in flight.

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

A comprehensive demo lives in [demos/signal_slot_demo.cpp](demos/signal_slot_demo.cpp) (`make demo-signals`): a typed emitter/observer architecture — per-type emitters, virtual observers with RAII subscriptions, multi-type emitters and observers wired with fold expressions — measured through fan-out, multi-type, cross-thread, and many-to-many 4×4 mesh scenarios, in both messages per second and checksummed GB/s.

## Building and testing

The library is header-only — just add the headers to your include path.

The unit tests are deliberately lightweight: plain `cassert`, no framework.

```sh
make test    # build and run the unit tests
make tsan    # the same tests under ThreadSanitizer
make asan    # the same tests under Address + UB Sanitizers
make demo    # build and run the demo (same code as the Xcode target)
make bench   # dependency-free throughput benchmarks (ring vs mutex/spin/cv baselines)

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

Measured on the reference machine — a 2014 Intel iMac, Apple Clang, `-O3` — via `make bench` and `make demo-signals`. Treat them as *relative* guidance and rerun on your own hardware; CI runs both harnesses on every push (informational: logged in the job output, never a pass/fail gate). Single-op SPSC numbers swing severalfold with thread placement (sibling hyperthreads share cache; separate cores bounce it) — the batched numbers are stable precisely because batching amortises that traffic.

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

## Where this is going

The roadmap — `synchronized<T>`, a moveable SPSC circular buffer with honest atomics, a disruptor, and a thread-safe signal/slot — lives in [FUTURE_DIRECTIONS.md](FUTURE_DIRECTIONS.md), along with the non-goals and the reasoning behind both.

## Caveats

- The quiescence probe on `moveable_mutex` and `moveable_spin_lock` is a `try_lock`; on a recursive mutex it cannot detect a lock held by the moving thread itself. Moving a mutex you yourself hold is still a logic error.
- A mutex/condition-variable move is safe against *stale* use, not against *concurrent* use: a thread may lock or wait immediately after the probe. Move objects when they are quiescent; the checks exist to make misuse loud.
- `moveable_barrier`'s completion function runs while the barrier's internal lock is held; it must not call back into the barrier (as with `std::barrier`, where that is undefined behaviour).
