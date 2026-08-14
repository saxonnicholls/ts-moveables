# Changelog

All notable changes to ts-moveables are recorded here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and
this project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
The version is written in `TSMoveables/version.hpp`, `CMakeLists.txt` and the
git tag, and `make check-version` fails if those three ever disagree.

## [1.1.0] — 2026-08-14

### Added

- `websocket_client` (`http/websocket_client.hpp`) — the **outbound** half of
  WebSocket, which is what makes the broadcast hub a *relay*: N upstream feeds
  in, one fan-out core, M browsers out, on one reactor. Auto-reconnect is the
  default, with exponential backoff and **full jitter** (undithered backoff
  resynchronises a fleet of relays into a thundering herd when a feed restarts),
  and the backoff counter is forgiven only by a session that outlives
  `session_grace` — connecting is not the same as working, and a server that
  accepts then drops must not reset the delay every attempt. `close()` is final,
  so shutdown never races the retry timer. RFC 6455 at this end: client frames
  are masked with a fresh key per frame (§5.3), a masked *inbound* frame fails
  the connection (§5.1), and `Sec-WebSocket-Accept` is verified rather than
  assumed — a plain `200 OK` is not an upgrade, and there is a test that says so.
  Survivability is tested by killing the upstream and requiring the client to
  come back on its own.

- `intern_pool<T, Hash, Eq>` (`concurrent/intern_pool.hpp`) — content-addressed
  interning ("hash-consing"): identical values collapse to a single shared,
  immutable instance, so equal data is stored once and compared by pointer.
  Self-cleaning via weak references (memory bounded by the live working set, not
  history), collision-safe (the hash selects a bucket, an exact `Eq` compare is
  the authority — distinct values never alias), and thread-safe. `find()` probes
  without allocating; `snapshot()` reports interned / hits / live.
- `ws_broadcast_hub::ingest_handler(topic)` — a fixed-topic ingest handler, so
  several independent webhook endpoints can run on one server
  (`srv.post("/hook1", hub.ingest_handler("hook1"))`, `"/hook2"`, …), each
  feeding its own topic and the wildcard firehose.
- `websocket::send_text_shared(shared_ptr<const std::string>)` — fan a single
  immutable payload out to many sockets without copying it into each send.

### Changed

- **`ws_broadcast_hub` fan-out no longer copies the frame per subscriber.** Each
  subscriber queue and the replay ring now hold a `shared_ptr<const std::string>`
  to one framed message instead of a private copy, and the pump uses
  `send_text_shared`. Publish cost stops scaling with payload × subscribers:
  in an isolated fan-out benchmark, ~6× faster at small frames and up to ~87×
  faster at 8 KB × 1000 subscribers (where it previously spent ~2 ms per publish
  purely copying). Behaviour is unchanged; only the copies are gone.
- **`intern_pool` is now moveable.** It shipped non-movable on the reasoning that
  "identity is the point" — which holds for *copy*, since two pools would fork
  the identity domain and `a == b` would stop implying `ptr_a == ptr_b`, but not
  for *move*, which relocates the one pool while every `shared_ptr<const T>`
  already handed out stays valid and canonical. A component of this library that
  could not be a member of a moveable object was arguing against the library's
  own thesis. It now holds a `moveable_mutex`, so the move is checked rather
  than trusted: moving a pool out from under a thread inside `intern()` throws
  instead of being undefined.
- The helpers shared out of the logger and the hub live in **`snicholls::utils`**,
  not `snicholls::detail`. The latter collided: any translation unit with both
  `using namespace snicholls;` and `using namespace snicholls::http;` — which
  every benchmark and demo here has — then saw two `detail` namespaces and every
  mention of one became ambiguous.

### Fixed

- `tests/tests_intern_pool.cpp` was missing from `CMakeLists.txt`, so the three
  CMake CI jobs would have failed at **link** time on the missing
  `run_intern_pool_tests()` symbol while `make test` (which globs) stayed green.
  Added, plus `scripts/check_test_registry.py` — run by `make check-tests` and on
  every CI job — so the list and the directory cannot drift apart again. It
  checks both directions: a file listed but deleted breaks `cmake` configure for
  everyone who clones.

## [1.0.0] — 2026-07-29

First tagged release. The library has been in development since 2010; this is
the point at which the API is committed to, not the point at which it began.

`find_package(ts_moveables)` is exported with `SameMajorVersion` compatibility,
so from here a breaking change to any public interface means 2.0.0.

### The one rule

Every synchronisation primitive in the C++ standard library is immovable, which
means any class holding one loses the rule of zero. This library makes them
moveable under a single contract — **move only when quiescent** — so a class
with a mutex, a condition variable or a lock-free ring inside it can go back to
being an ordinary value type.

### Moveable primitives

`moveable_atomic`, `moveable_mutex` (plain, recursive, timed, shared, shared
timed), `moveable_spin_lock`, `moveable_condition_variable(_any)`,
`moveable_once_flag` with `snicholls::call_once`, `moveable_semaphore`,
`moveable_latch`, `moveable_barrier`. Misuse is loud: moving a locked mutex or a
ring with a push in flight throws rather than corrupting silently.

### Concurrent containers and pools

- `synchronized<T, M>` / `synchronized_waitable<T, M>`, and the heterogeneous
  family (`synchronized_variant`, `synchronized_tuple`, `synchronized_any`,
  `synchronized_type_map`, `synchronized_bag`)
- `circular_buffer<T>` — bounded SPSC ring, runtime or compile-time capacity,
  with batch `push_n` / `pop_n`
- `mpmc_queue<T>` — bounded lock-free Vyukov MPMC ring
- `work_stealing_deque<T>` — bounded Chase-Lev
- `disruptor<T, WaitStrategy>` and `multi_producer_disruptor<T>` — the LMAX
  pattern with dependency graphs, batch consumption and publication, and three
  wait strategies. The producer discipline is a compile-time choice so the
  single-producer path pays nothing for the multi-producer bookkeeping.
- `task_pool` interface with five implementations: `mutex_`, `sharded_`,
  `dispatch_`, `mpmc_` and `work_stealing_task_pool`

### Signals

`moveable_signal<Args...>` with `connection` / `scoped_connection`. Emission
takes an immutable snapshot rather than holding a lock across user code, so
slots may connect, disconnect or re-emit freely. Weak-pointer target tracking
auto-disconnects. Connections survive the signal being moved.

### Event loop

`event_loop` with `fd_watch` and `timer` handles — epoll on Linux, kqueue on
macOS/BSD, `poll()` fallback. The loop handle itself moves while running.
`time_master` builds named, cancellable, repeating timers on top. POSIX only:
on Windows the header self-disables rather than shipping a pretend port, since
IOCP is a proactor and bridging the two models badly is how loops get baroque.

### HTTP server

A non-blocking reactor built on two runtime-chosen axes — transport delegate
(how bytes arrive) and protocol delegate (what they mean):

- **HTTP/1.1** — strict incremental parser (request-smuggling vectors rejected
  rather than tolerated), routing, keep-alive, chunked bodies,
  `Expect: 100-continue`, async responders, response streaming with
  backpressure, timeouts, `EMFILE` guard
- **HTTP/2** — framing, HPACK with compile-time Huffman tables, stream state
  machine, bidirectional flow control, multiplexing, ALPN, and the abuse limits
  designed in rather than added after an incident. **h2spec 2.6.0: 147/147.**
- **WebSocket** — RFC 6455 with `permessage-deflate` (RFC 7692).
  **Autobahn 25.10.1: 517/517**, compression groups included.
- **TLS** — two backends, OpenSSL and mbedTLS 3.x, both driven through memory
  BIOs so the engine never touches a socket. `wss://` required no code: it is
  WebSocket over the TLS transport, and the two never meet.

### Logging and telemetry

`logging/logger.hpp` — log from any thread with no locks and no logger threaded
through the call graph. `LOG()` stamps a record and returns; independent
**lanes** each own a queue and drain thread, so a slow sink cannot stall a fast
one. Per-lane overflow policy with drops counted rather than hidden, a total-order
sequence stamp (timestamps alone cannot order concurrent logging), an optional
reordering window, a persistent sequence that survives restart, and a
journal/replayer pair. Telemetry rides the same pipeline.

### Packaging

Header-only. `#include "ts_moveables.hpp"`, or copy one amalgamated file from
`single_include/` — `ts_moveables.hpp` for the whole library, `ts_http_server.hpp`
for the server. CMake `add_subdirectory`, `FetchContent` and
`find_package(ts_moveables)` all supported.

### Verified

141 unit tests at both C++17 and C++20, across Linux (x86-64 and ARM64, GCC and
Clang), macOS (Apple Silicon and Intel) and Windows (MSVC), with ThreadSanitizer
on every POSIX platform and Address + UB Sanitizers on Linux. Both TLS backends
built and run on every POSIX job. External graders: h2spec 147/147, Autobahn
517/517, and the RFC 7541 Appendix C HPACK vectors including the eviction series.

Every third-party version is pinned — mbedTLS by version *and* SHA-256, the
graders and benchmark peers by tag — so a result is reproducible and an upstream
change cannot silently move a published number.

### Known limits, stated plainly

- POSIX only for the event loop and HTTP server; `SNICHOLLS_HAS_EVENT_LOOP` and
  `SNICHOLLS_HAS_HTTP_SERVER` are 0 on Windows. The primitives and containers
  are portable and build everywhere.
- No HTTP/3. Planned as a *wrapper* around a QUIC library, never a QUIC
  implementation of our own.
- HTTP/2 does not implement `Upgrade: h2c` (removed by RFC 9113), server push
  (`ENABLE_PUSH=0` is advertised), CONNECT, or PRIORITY beyond parse-and-ignore.
- A whole-body `respond()` is bounded by what the application chose to allocate.
  Streamed responses are bounded by `h2_config::max_outbound_buffer`.
- The moodycamel queues are faster per single operation; use this library's
  batch APIs, or moodycamel, when that is the bottleneck. The gap and the reason
  for it are documented rather than hidden.

[1.1.0]: https://github.com/saxonnicholls/ts-moveables/releases/tag/v1.1.0
[1.0.0]: https://github.com/saxonnicholls/ts-moveables/releases/tag/v1.0.0
