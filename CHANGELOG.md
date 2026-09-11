# Changelog

All notable changes to ts-moveables are recorded here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and
this project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
The version is written in `TSMoveables/version.hpp`, `CMakeLists.txt` and the
git tag, and `make check-version` fails if those three ever disagree.

## [Unreleased]

### Added

- **`origin_policy::allows(const request&)` and
  `ws_broadcast_hub::origin_allowed(const request&)`** — the 1.1.2 `Origin`
  check as one call from inside a handler. It guards the handlers `mount()`
  registers and nothing else, which reads more broadly than it is: routes match
  in registration order and first match wins, so a consumer's own route on the
  same path registered *before* `mount()` shadows the hub's guarded one; and
  `publish()` takes a topic and bytes, with no request to read an `Origin` from,
  so it cannot check one. Both are by design, and together they mean a
  hand-written ingest route is guarded by its author, not by the hub. Found the
  way these things are: a downstream consumer (super-log) took 1.1.2, confirmed
  `/ws` was refusing foreign origins, and found its own `/ingest` route still
  publishing them — the check was working and the request was never reaching it.
  Documented on `mount()`, in the header comment and in the README, because the
  fix is one line and the failure is silent. No behaviour change.

## [1.1.2] — 2026-09-11

A security release. The one fix that names it is the `Origin` check below —
everything else had been sitting in `[Unreleased]` and ships alongside it.

### Security

- **A WebSocket upgrade accepted any `Origin`, so any web page could read a
  loopback server's streams.** This is the one asymmetry that catches nearly
  everyone: browsers do *not* apply the same-origin policy to WebSockets. A
  `fetch()` to `http://127.0.0.1:7333/` is stopped before it leaves the page; a
  WebSocket to `ws://127.0.0.1:7333/` is not. So any page the developer happened
  to visit could open `ws://127.0.0.1:<port>/ws?topic=*` against a
  loopback-bound `ws_broadcast_hub`, and read every stream on it — with
  `replay_on_connect`, the backlog too. Binding to loopback reads like a
  boundary and is not one: the browser is already on the loopback side, and the
  browser is what is asking. Found by an independent security review of a
  downstream consumer (super-log), which completed a raw RFC 6455 handshake
  carrying a foreign `Origin` against a loopback hub and got `101` plus live
  frames; the exposure there was OS logs, ssh auth failures, git, DNS and
  outbound connections, on a fixed and documented port.

  The handshake is the only place that can refuse this, and the only evidence it
  has is the `Origin` header — worth exactly as much as the fact that a browser
  attaches it to every cross-origin socket and will not let script take it off.
  New `http::origin_policy` (`http/config.hpp`), carried on `ws_config::origin`
  and `ws_broadcast_hub::config::origin`, checked in `websocket_route` **before
  the 101** and in the hub's ingest handler before the publish:

  - no `Origin` header at all → **allow**. curl, a native client, a webhook
    sender: no browser, so no drive-by, and none of them send one.
  - loopback `Origin` (`localhost`, all of `127.0.0.0/8`, `::1`) → **allow**.
    A page served from localhost is the tool's own console, which is the common
    shape for a local server and the case a blanket reject breaks.
  - listed in `origin.allow` → **allow**. The deployed browser app's real
    origin, matched as one exact serialised origin, case-blind, no wildcards.
  - anything else → **403, no upgrade**. `null` is not loopback and not
    special: it names nobody, so it faces the allowlist like any other value.

  Lookalikes are refused by construction, because this is the check that always
  gets caught by them: the host is parsed out and matched whole, never by
  substring or suffix, and a value carrying a path, userinfo, query or fragment
  is not an origin at all and is refused rather than read generously — so
  `http://localhost.evil.example`, `http://127.0.0.1.evil.example` and
  `http://evil.example/@localhost` are all rejected.

  **The same policy closes the write side**, which the review raised separately
  as CSRF into `/ingest`. A cross-origin `no-cors` POST with a `text/plain` body
  is a "simple request": it crosses with no preflight to refuse, and the page
  never needs to read the response to have injected a forged event. Answering
  with CORS headers was never the protection. But the browser attaches `Origin`
  to that POST too, so checking it is sufficient — and sufficient *without the
  endpoint speaking CORS at all*, which means a publisher using `no-cors`
  deliberately keeps working from an allowed origin, unchanged.

  What this does **not** claim: no-Origin is allowed, so it is not a defence
  against a non-browser process already on the loopback side. That process has
  local code execution, which is a boundary this check was never at.

  **This changes a default and can refuse connections that previously
  succeeded** — deliberately, since the old default was the vulnerability. A
  browser app on a real domain adds that origin to `origin.allow`; a server that
  genuinely wants any origin says so once with `origin_policy::any()` rather
  than being talked out of the default one exception at a time. Five tests,
  including the review's own repro and its control: the same foreign `Origin`
  that is refused `403` by default gets `101` under `any()`, so the tests grade
  the check rather than observing a broken connection.

### Added

- **`wss://` on `websocket_client`**, via a `transport_factory` on the config —
  the same two-axis split the server uses, so the client header still does not
  know what TLS is. `openssl_client_context` is the OpenSSL half:
  `TLS_client_method`, TLS 1.2 floor, SNI, optional ALPN.
  **Peer verification is on by default** and must be disabled by name
  (`insecure_skip_verify`). Both required checks are made — the chain must be
  trusted *and* the certificate must match the host dialled, the second being
  the one usually forgotten and exactly what an interception proxy exploits.
  A `wss://` URL with no transport is refused rather than downgraded to
  plaintext. Four tests, and the two refusal tests were confirmed to fail when
  verification is switched off, so they test the check rather than observing a
  broken connection.
- `make bench-wss` — ws against wss on the same path, swept by payload size,
  and in CI. The ratio is **0.58–0.88**, best at small payloads and worst at
  large ones, which falsifies the obvious prediction: the fixed per-record cost
  is not what dominates, per-byte encryption is.
- `transport_delegate::start()` — the outbound case. A server transport is
  driven by bytes that arrive; a client must send the ClientHello before there
  is anything to react to. Default no-op.

### Fixed

- **`ws_broadcast_hub`'s replay ring was unbounded in bytes.** `ring_capacity`
  bounded the ring by chunk *count* while the subscriber queues beside it were
  bounded by count and bytes - and a chunk is whatever one producer batched,
  up to `max_message_bytes`, so the worst case was `ring_capacity` x 1MB per
  topic. Not theoretical: a 20-minute soak feeding one topic at ~290 frames/s
  (super-log against 250 Binance streams) held 66MB of live reachable heap in
  a single ring and climbed ~3MB/min, with `leaks` reporting zero - every
  byte was still referenced, which is exactly why no leak tool would ever
  have caught it. The ring now carries a byte budget, `config::ring_bytes`
  (default 8MB, mirroring `max_queue_bytes`; 0 restores count-only), evicting
  oldest-first until under budget but never the frame just pushed, so a chunk
  larger than the whole budget still replays itself. Quiet topics keep their
  full count-depth replay; only oversized history is trimmed. `stats` gains
  `ring_bytes` - the number that would have shown this growth on any
  dashboard - and the new unit test proves the bound holds, the survivors are
  the newest, and the surviving run is gapless.

- **`websocket_client` memmoved its whole read buffer once per frame.** The
  inbound path consumed frames with `in.erase(0, n)`, which shifts everything
  behind the frame just consumed - so draining a window full of small frames
  cost O(bytes x frames), quadratic in the batch. It now consumes with an
  offset and compacts only when the consumed prefix is worth a single memmove,
  the same idiom `h2_stream::out_buf` and the hub's queues already used.
  Measured on the ws/wss benchmark: **+42% at 1 KB, +61% at 4 KB, +87% at
  16 KB**, taking peak throughput from 838 MB/s to **1,571 MB/s**.
- **`websocket_client` gave up on the first resolved address.** A non-blocking
  `connect()` to a dead address returns `EINPROGRESS` exactly like a live one -
  the refusal only surfaces later through `SO_ERROR` - so taking the first
  address that did not fail immediately silently commits to the wrong one. A
  host with both AAAA and A records where only one is listening therefore
  failed every time; `localhost` resolving to `::1` ahead of `127.0.0.1` is the
  everyday version, and it is how this was found. The client now keeps the full
  candidate list and falls through to the next address on refusal.

## [1.1.1] — 2026-08-16

### Fixed

- **CI hardening found while shipping this release.** Autobahn was being graded
  *twice* — its `if:` matched both x86-64 Linux jobs while the comment above it
  said once was enough — and the step is now bounded (`timeout-minutes: 20`)
  after four runs where it hung and took the whole runner down with it, losing
  every other result on that machine. The step also now distinguishes "the
  grader could not run" from "the grader found failures", which
  `run_autobahn.sh` has always reported separately but the workflow collapsed
  together. The hang itself is undiagnosed and recorded in FUTURE_DIRECTIONS §11,
  along with two other gaps in the gates: the amalgamation check compiles only a
  one-line HTTP/1.1 program (which is why it never noticed the missing HTTP/2),
  and a local Autobahn run reaches 404 of the 517 cases CI grades, so passing
  locally is not evidence about that suite.

- **`http/http2.hpp` was missing from the umbrella header**, so HTTP/2 — graded
  147/147 by h2spec and headlined in the README — was unreachable both from
  `#include "ts_moveables.hpp"` and from the amalgamated `single_include/`
  drop-in that the README tells people to copy. Nothing was wrong with the
  HTTP/2 code; it simply was not wired into the two entry points most users
  consume, and no comment anywhere claimed the omission was deliberate. Caught
  by auditing the release assets rather than by a test, which is why
  `check-amalgamate` now has company: the drop-in smoke test compiled a
  one-line HTTP/1.1 program and so could never have noticed.

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

[1.1.2]: https://github.com/saxonnicholls/ts-moveables/releases/tag/v1.1.2
[1.1.1]: https://github.com/saxonnicholls/ts-moveables/releases/tag/v1.1.1
[1.1.0]: https://github.com/saxonnicholls/ts-moveables/releases/tag/v1.1.0
[1.0.0]: https://github.com/saxonnicholls/ts-moveables/releases/tag/v1.0.0
