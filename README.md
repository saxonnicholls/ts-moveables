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

## Building and testing

The library is header-only — just add the headers to your include path.

The unit tests are deliberately lightweight: plain `cassert`, no framework.

```sh
make test    # build and run the unit tests
make tsan    # the same tests under ThreadSanitizer
make asan    # the same tests under Address + UB Sanitizers
make demo    # build and run the demo (same code as the Xcode target)

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

## Caveats

- The quiescence probe on `moveable_mutex` and `moveable_spin_lock` is a `try_lock`; on a recursive mutex it cannot detect a lock held by the moving thread itself. Moving a mutex you yourself hold is still a logic error.
- A mutex/condition-variable move is safe against *stale* use, not against *concurrent* use: a thread may lock or wait immediately after the probe. Move objects when they are quiescent; the checks exist to make misuse loud.
- `moveable_barrier`'s completion function runs while the barrier's internal lock is held; it must not call back into the barrier (as with `std::barrier`, where that is undefined behaviour).
