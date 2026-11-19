//
//  collective.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - collectives over TCP for data-parallel training:
//  N ranks, one full mesh, an allreduce whose sum is the same bits on every
//  rank and the same bits as a sequential sum in rank order.
//
//      snicholls::net::collective_config cfg;
//      cfg.rank   = 2;                                        // this machine
//      cfg.peers  = {"10.0.0.1:7100", "10.0.0.2:7100", "10.0.0.3:7100"};   // every rank, in rank order
//      cfg.run_id = 0x5eed;                                   // the same on every rank of one job
//      auto c = snicholls::net::collective::connect(cfg);     // result<collective, collective_error>
//      if (c.is_err()) { std::cerr << c.unwrap_err_unchecked().what() << "\n"; return 1; }
//
//      c->broadcast(weights.data(), weights.size(), 0);       // rank 0's initial weights everywhere
//      c->allreduce_sum(grads.data(), grads.size());          // every rank: the same sum, bit for bit
//      c->allreduce_sum_bf16(bits.data(), bits.size());       // the same over bfloat16, half the bytes
//      c->barrier();
//
//  ------------------------------------------------------------- the sum
//
//  allreduce_sum(data, n) leaves, on every rank, per element
//
//      ((x_0 + x_1) + x_2) + ... + x_{N-1}
//
//  added left to right in RANK order, in float. Not pairwise, not in the order
//  a ring happens to visit the ranks: one fixed order, so the result does not
//  depend on N's factorisation, on timing, or on which rank finished first, and
//  a single-machine reference (snicholls-core's ml::ordered_sum computes the
//  same thing) can check it bit for bit. Floating-point addition is not
//  associative, so this is a real constraint: a ring allreduce gives each
//  element a different summation order depending on which rank owns its chunk.
//
//  allreduce_sum_bf16(bits, n) is the same over bfloat16 values (the 16-bit
//  patterns of snicholls-core's ml::bfloat16): every rank ends with, per element,
//
//      bf16( ((f(x_0) + f(x_1)) + f(x_2)) + ... + f(x_{N-1}) )
//
//  where f decodes a bfloat16 to float exactly, the sum is in float in rank
//  order (core's ml::ordered_sum_bf16), and bf16 rounds it to nearest even
//  (core's ml::to_bfloat16) once, at the rank that owns the chunk. bfloat16
//  travels both ways - the pieces to the owner and the reduced chunks back - so
//  each rank moves exactly half the bytes of allreduce_sum for the same n (the
//  40-byte headers aside). The sum's one rounding is the price; a caller that
//  divides by N for a mean does so after (core's ml::ordered_mean_bf16 is that
//  mean). NaN comes back quiet; infinities add as float adds them.
//
//  ------------------------------------------------------------ the algorithm
//
//  The buffer is cut into N chunks; chunk c is owned by rank c (the first n % N
//  chunks are one element longer). Every rank sends its piece of chunk c
//  directly to rank c; the owner adds the N pieces of its chunk in rank order,
//  its own piece at its own position; then every owner sends its reduced chunk
//  to every other rank. Each rank sends 2(N-1)/N of the buffer and receives the
//  same - what a ring moves - in two steps rather than 2(N-1), with every link
//  busy in both directions at once.
//
//  It streams. The owner reduces its chunk in segments (segment_bytes, 1 MiB by
//  default) as the pieces arrive, holding at most two segments per peer, and
//  sends each reduced segment on as soon as it is done, so reduce-scatter and
//  all-gather overlap and the extra memory is about 2 (N-1) segment_bytes,
//  whatever the buffer size. Sends go straight from the caller's buffer, and the
//  all-gather lands straight in it. One thread, one poll() loop over every peer's
//  socket, non-blocking in both directions: two ranks sending each other half a
//  gigabyte cannot deadlock on full socket buffers.
//
//  broadcast(data, n, root) scatters the buffer from the root in N-1 pieces, one
//  to each other rank, which forward their piece to everyone but the root as it
//  arrives: the root sends the buffer once, not N-1 times. barrier() returns once
//  every rank has entered it.
//
//  ---------------------------------------------------------------- the mesh
//
//  connect() makes two TCP connections per pair of ranks, one for data and an
//  idle one for abort notices (below): rank r listens at peers[r] and dials
//  every lower rank, retrying while they start (backoff from 20 ms to 500 ms)
//  until connect_timeout. The handshake carries the
//  world size, both ranks, the run id and the byte order, and either side
//  refuses a mismatch, so two jobs on the same ports, or one job started with
//  two different peer lists, never exchange a gradient. A listener can also be
//  passed in already bound (port 0 in tests, so ports never collide).
//
//  -------------------------------------------------------------- failures
//
//  Nothing waits forever. connect() is bounded by connect_timeout. Every
//  collective fails with timed_out naming the peer when a transfer that is
//  waiting on that peer moves no byte for io_timeout - a stall deadline rather
//  than a deadline for the whole call, because half a gigabyte over 1 Gbit/s
//  takes seconds and a frozen peer should not. A peer that dies shows as
//  peer_closed (or io_error) naming it, as soon as its socket says so. Every
//  collective starts with a 32-byte header to each peer (operation, element
//  count, root, sequence number); a rank that called something else, or with a
//  different n, is reported as mismatched_call.
//
//  Every rank blames the same peer. A rank whose collective fails first sends
//  every other rank an 8-byte notice on the control connection - the error
//  code and the rank it blames - and they fail at once with that blame. Without
//  it, when rank 2 dies, rank 1 may see rank 0 give up and close before it sees
//  rank 2's own connection end, and blame rank 0. A connection that ends is
//  blamed only after a short look (100 ms at most) for such a notice.
//
//  After any failure the collective is broken: the streams may be mid-message,
//  so every later call returns `broken`. Destroy it (that closes the sockets)
//  and connect again - or, in an elastic collective, reform() (below).
//
//  A slow link is not a stall. A rank whose last bytes sit in its socket and
//  tunnel buffers finishes a collective and waits in the next one while its
//  peer is still receiving the last; over a slow, buffering link (an ssh tunnel
//  at 0.1 Mbit/s) that wait can outlast io_timeout with nothing wrong. So a
//  rank moving bytes in a collective sends every other rank an 8-byte progress
//  notice on the control connection, at most every io_timeout / 4, and a rank
//  that hears one restarts its stall clock for that peer. A frozen or dead peer
//  sends none, and is still named at io_timeout.
//
//  ---------------------------------------------------------- re-formation
//
//  With cfg.elastic, a failure need not end the job. cfg.rank is then the
//  rank's slot, its fixed place in cfg.peers; the membership is a list of
//  slots and an epoch (0 at connect), and rank() is the slot's position in the
//  list. Every rank listens, the last too, and keeps its listener.
//
//  - reform(), on every survivor after a failure: each dials the members of the
//    old epoch below its own slot, lowest first, and registers with the first
//    that answers (naming the rank its failure blamed). One that finds no lower
//    member alive is the coordinator. It waits until every old member has
//    registered or has been blamed by a registered one - at once, usually: the
//    abort notices make every survivor blame the same rank - or until
//    gather_window, then sends each registered survivor the new membership:
//    the survivors in slot order, epoch + 1. Ranks are dense again, in the old
//    order. A survivor too late for that is told it is excluded, and joins.
//  - join(), on a restarted rank: it dials the other slots, lowest first, with
//    the run id, until the rendezvous - rank 0 of the current epoch, the lowest
//    live slot - holds it. Another run id is refused. It waits there.
//  - admit(), collective, on every member at a round boundary: rank 0 says which
//    joiners it holds (a small broadcast, so it is a barrier too). With any, the
//    members and the joiners move to epoch + 1 together. Joiners enter only
//    here, never in the middle of a collective.
//  - poll(), between collectives (each training step): sees a failure without
//    waiting for the next collective (a notice, a closed connection, a survivor
//    registering) and lets the rendezvous take joiners' requests; a group
//    re-forming waits for every survivor's next poll().
//
//  Every mesh connection's handshake carries the epoch and a digest of the
//  members, and every op header the epoch, so two ranks holding different
//  memberships never connect, let alone run a collective together: every
//  collective in an epoch is the reference over that epoch's members. What is
//  agreed is the membership, not the data: when a rank dies at the very end of
//  a collective, some survivors may have completed it and others not. The
//  caller settles which rounds count (ts-moveables cannot: it does not know what
//  a round is) - for example with a small allreduce after the re-formation, in
//  which each member says what it holds.
//
//  Limits: the coordinator is chosen by who answers, so a network partition
//  (not a death) can split the group in two, each re-forming on its own;
//  min_members (a majority, say) refuses the smaller side. A coordinator that
//  dies during a re-formation leaves its registrants with an error (they call
//  reform() again). Deadlines: probe_timeout to dial one rank, gather_window,
//  reform_timeout for the whole of reform(), join_timeout for join().
//
//  Results are snicholls::result<T, collective_error>, as in fs/ and
//  http::client; collective_error is a std::runtime_error whose what() names
//  the peer, with code() and peer() to branch on.
//
//  ------------------------------------------------------------------ limits
//
//  - Every rank must have the same float format and byte order; the handshake
//    checks the byte order. Built with -ffast-math this header refuses to
//    compile: it may reassociate the sum.
//  - One thread at a time per collective. Ranks are processes (or threads)
//    each with their own collective.
//  - The handshake is version 2 (56 bytes, with the epoch and the members'
//    digest), so ranks of 1.9.0 and later releases do not mesh together.
//  - No encryption or authentication beyond the run id: run it on a network
//    you trust.
//
//  C++20 and POSIX (Linux and macOS), like fs/ and http::client: opt-in, not
//  in the single header. Tests: `make test-collective`; demo:
//  `make demo-collective`; benchmark: `make bench-collective`.
//

#ifndef ts_moveables_net_collective_hpp
#define ts_moveables_net_collective_hpp

#if !defined(_WIN32)

#include <algorithm>
#include <array>
#include <cerrno>
#include <cfloat>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <poll.h>
#include <sys/socket.h>

#include <snicholls/core/backoff.hpp>
#include <snicholls/core/ml/minifloat.hpp>
#include <snicholls/core/result.hpp>

#include "tcp.hpp"

#if defined(__FAST_MATH__)
#error "net/collective.hpp: allreduce_sum promises a rank-order float sum, which -ffast-math may reassociate"
#endif
#if defined(FLT_EVAL_METHOD) && FLT_EVAL_METHOD != 0
#error "net/collective.hpp: float sums must be evaluated in float (FLT_EVAL_METHOD == 0) to be the same bits everywhere"
#endif

namespace snicholls {
namespace net {

static_assert(std::numeric_limits<float>::is_iec559 && sizeof(float) == 4,
              "snicholls::net::collective sends IEEE 754 binary32 floats");

enum class collective_errc : std::uint8_t {
    bad_config = 1,       // a rank, endpoint, buffer or option that cannot work
    bind_failed,          // this rank could not listen at its own endpoint
    connect_failed,       // a lower rank could not be reached within connect_timeout
    handshake_rejected,   // a peer of another job, world size, run id or byte order
    timed_out,            // no byte moved with a peer for io_timeout (connect_timeout while connecting)
    peer_closed,          // the peer's end of the connection went away
    io_error,             // a socket error other than the peer closing
    mismatched_call,      // the peer called a different collective, or with a different n or root
    broken,               // an earlier call failed; reform() (elastic) or connect again
    excluded,             // the group re-formed without this rank, or refused it: join() again
};

inline const char* to_string(collective_errc e) noexcept
{
    switch (e) {
    case collective_errc::bad_config: return "bad configuration";
    case collective_errc::bind_failed: return "could not listen";
    case collective_errc::connect_failed: return "could not connect";
    case collective_errc::handshake_rejected: return "handshake rejected";
    case collective_errc::timed_out: return "timed out";
    case collective_errc::peer_closed: return "peer closed the connection";
    case collective_errc::io_error: return "socket error";
    case collective_errc::mismatched_call: return "mismatched collective call";
    case collective_errc::broken: return "collective broken by an earlier failure";
    case collective_errc::excluded: return "not a member of the group";
    }
    return "?";
}

/// A failed connect or collective. peer() is the rank at fault (-1 when no
/// single peer is), and what() names it with its endpoint.
class collective_error : public std::runtime_error {
public:
    collective_error(collective_errc code, int peer, const std::string& detail)
        : std::runtime_error(std::string(to_string(code)) + (detail.empty() ? "" : ": " + detail)),
          code_(code), peer_(peer)
    {
    }
    collective_errc code() const noexcept { return code_; }
    int peer() const noexcept { return peer_; }

private:
    collective_errc code_;
    int peer_;
};

template <class T>
using collective_result = result<T, collective_error>;

struct collective_config {
    int rank = 0;                         // this rank, 0 <= rank < peers.size()
    std::vector<std::string> peers;       // "host:port" (or "[v6]:port") of every rank, in rank order
    std::uint64_t run_id = 0;             // the same on every rank of one job, different between jobs
    std::string listen_host;              // where this rank's listener binds; empty: every interface
    std::chrono::milliseconds connect_timeout{60'000};    // the whole of connect(), retries included
    std::chrono::milliseconds handshake_timeout{5'000};   // one accepted connection's hello
    std::chrono::milliseconds io_timeout{30'000};         // a collective fails after this long with no progress
    std::size_t segment_bytes = std::size_t{1} << 20;     // the reduction's streaming unit; memory is ~2(N-1) of these
    int socket_buffer_bytes = 0;          // SO_SNDBUF and SO_RCVBUF; 0 leaves the kernel's autotuning alone

    // Elastic membership (see "re-formation" in the header comment). Off: a failure breaks the
    // collective for good, as before. On: every rank listens (the last too), a broken collective
    // can reform() among the survivors, a restarted rank can join() a running group, and the group
    // admits joiners at admit(). `rank` is then the rank's slot: its fixed place in `peers`.
    bool elastic = false;
    std::chrono::milliseconds probe_timeout{2'000};      // dialling one rank that may be gone
    std::chrono::milliseconds gather_window{120'000};    // how long a coordinator waits for a member neither
                                                         // registered nor blamed; longer than the time between
                                                         // two poll()s (an inner step of training)
    std::chrono::milliseconds reform_timeout{300'000};   // the whole of reform()
    std::chrono::milliseconds join_timeout{3'600'000};   // the whole of join(): finding the group and being admitted
    std::size_t min_members = 1;                         // reform() fails below this many survivors
};

namespace detail {
namespace collective {

using clock = std::chrono::steady_clock;

struct endpoint {
    std::string host;
    std::uint16_t port = 0;
};

// "host:port", "[v6]:port". The port is required and not zero.
inline std::optional<endpoint> parse_endpoint(std::string_view s)
{
    endpoint e;
    std::string_view port;
    if (!s.empty() && s.front() == '[') {
        const auto close = s.find(']');
        if (close == std::string_view::npos || close + 1 >= s.size() || s[close + 1] != ':')
            return std::nullopt;
        e.host = std::string(s.substr(1, close - 1));
        port = s.substr(close + 2);
    } else {
        const auto colon = s.rfind(':');
        if (colon == std::string_view::npos || s.find(':') != colon)
            return std::nullopt;
        e.host = std::string(s.substr(0, colon));
        port = s.substr(colon + 1);
    }
    if (e.host.empty() || port.empty() || port.size() > 5)
        return std::nullopt;
    std::uint32_t v = 0;
    for (const char ch : port) {
        if (ch < '0' || ch > '9')
            return std::nullopt;
        v = v * 10 + std::uint32_t(ch - '0');
    }
    if (v == 0 || v > 65535)
        return std::nullopt;
    e.port = std::uint16_t(v);
    return e;
}

inline void put32(unsigned char* p, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) p[i] = static_cast<unsigned char>(v >> (8 * i));
}
inline void put64(unsigned char* p, std::uint64_t v)
{
    for (int i = 0; i < 8; ++i) p[i] = static_cast<unsigned char>(v >> (8 * i));
}
inline std::uint32_t get32(const unsigned char* p)
{
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
inline std::uint64_t get64(const unsigned char* p)
{
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

// ------------------------------------------------------------- the handshake
//
// 56 bytes each way, little-endian except the byte-order probe, which is the
// sender's native 0x01020304 so a peer of the other byte order reads it wrong.
// `channel` says which of the pair's two connections this is: 0 carries the
// data, 1 only ever carries an abort notice. `from` and `to` are slots (a rank's
// fixed place in the peers list); `epoch` and `digest` name the membership the
// sender holds, and a mesh connection between two different views is refused.
//
// The same frame carries the elastic requests on a rank's listener: register
// (a survivor to the rank it takes for the coordinator; `to` is the slot it
// blames), join (a restarted rank to the one it takes for the rendezvous), and
// the answers wait (held until the next admit) and view (the new membership,
// followed by `world` slots, 4 bytes each).
inline constexpr std::uint32_t hello_magic = 0x48534d54;   // "TMSH"
inline constexpr std::uint16_t hello_version = 2;
inline constexpr std::size_t hello_size = 56;
enum : unsigned char { hello_kind = 0, accept_kind = 1, reject_kind = 2, register_kind = 3, join_kind = 4,
                       wait_kind = 5, view_kind = 6 };
enum : unsigned char { ok_reason = 0, bad_magic, bad_world, bad_rank, duplicate_rank, bad_run_id, bad_byte_order,
                       bad_view, not_coordinator, stale_epoch, too_few };

inline const char* reason_text(unsigned r)
{
    switch (r) {
    case bad_magic: return "not a collective of this version";
    case bad_world: return "world size differs";
    case bad_rank: return "rank out of place";
    case duplicate_rank: return "rank already connected";
    case bad_run_id: return "run id differs";
    case bad_byte_order: return "byte order differs";
    case bad_view: return "a different membership (epoch or members)";
    case not_coordinator: return "not the group's coordinator";
    case stale_epoch: return "the group has re-formed without this rank";
    case too_few: return "too few members survived";
    default: return "refused";
    }
}

struct hello {
    unsigned char kind = hello_kind;
    unsigned char reason = ok_reason;
    unsigned char channel = 0;
    std::uint32_t world = 0, from = 0, to = 0;
    std::uint64_t run_id = 0;
    std::uint64_t epoch = 0, digest = 0;
    bool native_order = true;
    bool magic_ok = true;
};

inline std::array<unsigned char, hello_size> encode(const hello& h)
{
    std::array<unsigned char, hello_size> b{};
    put32(b.data(), hello_magic);
    b[4] = static_cast<unsigned char>(hello_version & 0xff);
    b[5] = static_cast<unsigned char>(hello_version >> 8);
    b[6] = h.kind;
    b[7] = h.reason;
    put32(b.data() + 8, h.world);
    put32(b.data() + 12, h.from);
    put32(b.data() + 16, h.to);
    put64(b.data() + 20, h.run_id);
    const std::uint32_t probe = 0x01020304;
    std::memcpy(b.data() + 28, &probe, 4);
    b[32] = h.channel;
    put64(b.data() + 36, h.epoch);
    put64(b.data() + 44, h.digest);
    return b;
}

inline hello decode(const unsigned char* b)
{
    hello h;
    h.magic_ok = get32(b) == hello_magic && (unsigned(b[4]) | unsigned(b[5]) << 8) == hello_version;
    h.kind = b[6];
    h.reason = b[7];
    h.world = get32(b + 8);
    h.from = get32(b + 12);
    h.to = get32(b + 16);
    h.run_id = get64(b + 20);
    std::uint32_t probe = 0;
    std::memcpy(&probe, b + 28, 4);
    h.native_order = probe == 0x01020304;
    h.channel = b[32];
    h.epoch = get64(b + 36);
    h.digest = get64(b + 44);
    return h;
}

// A membership's name: FNV-1a over the run id, the epoch and the slots in rank order
inline std::uint64_t view_digest(std::uint64_t run_id, std::uint64_t epoch, const std::vector<std::uint32_t>& slots)
{
    std::uint64_t h = 0xcbf29ce484222325ull;
    auto mix = [&h](std::uint64_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            h ^= (v >> (8 * i)) & 0xff;
            h *= 0x100000001b3ull;
        }
    };
    mix(run_id, 8);
    mix(epoch, 8);
    mix(slots.size(), 4);
    for (const auto s : slots)
        mix(s, 4);
    return h;
}

// ---------------------------------------------------------- the abort notice
//
// 8 bytes on the control connection, sent by a rank whose collective just
// failed, to every other rank, before it closes anything: the error code and
// the rank it blames. The other ranks fail at once with the same blame,
// rather than discovering the failure later through the closing rank's
// sockets and blaming it - which is what happens without this when one rank
// dies and another gives up first.
inline constexpr std::uint16_t abort_magic = 0x4154;          // "TA"
// The same 8 bytes with this magic is a progress notice: "I am moving bytes", sent by a rank in a
// collective to every other rank at most every io_timeout / 4 while bytes move. A rank waiting on
// that one restarts its stall clock: a peer still draining an earlier collective over a slow,
// buffering link is busy, not stalled.
inline constexpr std::uint16_t progress_magic = 0x5054;       // "TP"
inline constexpr std::size_t abort_size = 8;
inline constexpr std::uint32_t nobody = 0xffffffff;

// ----------------------------------------------------------- the op header
//
// 40 bytes to every peer at the start of every collective, little-endian: the
// operation, the element type, the root, the world, the sequence number within
// the epoch, the element count and the epoch.
inline constexpr std::uint32_t op_magic = 0x4f4d5354;      // "TSMO"
inline constexpr std::size_t op_header_size = 40;
enum : unsigned char { op_allreduce_sum = 1, op_broadcast = 2, op_barrier = 3 };
enum : unsigned char { dtype_none = 0, dtype_f32 = 1, dtype_bf16 = 2 };

inline const char* op_name(unsigned op)
{
    switch (op) {
    case op_allreduce_sum: return "allreduce_sum";
    case op_broadcast: return "broadcast";
    case op_barrier: return "barrier";
    default: return "an unknown operation";
    }
}

struct op_header {
    unsigned op = 0, dtype = 0;
    std::uint32_t root = 0, world = 0;
    std::uint64_t seq = 0, n = 0, epoch = 0;
};

inline std::array<unsigned char, op_header_size> encode(const op_header& h)
{
    std::array<unsigned char, op_header_size> b{};
    put32(b.data(), op_magic);
    b[4] = static_cast<unsigned char>(h.op);
    b[5] = static_cast<unsigned char>(h.dtype);
    put32(b.data() + 8, h.root);
    put32(b.data() + 12, h.world);
    put64(b.data() + 16, h.seq);
    put64(b.data() + 24, h.n);
    put64(b.data() + 32, h.epoch);
    return b;
}

inline std::string describe(const unsigned char* b)
{
    if (get32(b) != op_magic)
        return "bytes that are not a collective header";
    std::string s = op_name(b[4]);
    if (b[4] == op_allreduce_sum)
        s += b[5] == dtype_bf16 ? " (bfloat16)" : " (float)";
    s += " #" + std::to_string(get64(b + 16)) + " of " + std::to_string(get64(b + 24)) + " elements";
    s += " in epoch " + std::to_string(get64(b + 32));
    if (b[4] == op_broadcast)
        s += " from root " + std::to_string(get32(b + 8));
    return s;
}

// --------------------------------------------------------------- the engine
//
// A transfer is a list of parts per peer and direction, done in order. A part
// may be gated: `limit` points at a count of bytes that may move so far (a
// reduced prefix, a received prefix to forward, a ring with free space). A
// `ring` part writes at done % ring.
struct part {
    char* base = nullptr;
    std::size_t len = 0;
    std::size_t done = 0;
    const std::size_t* limit = nullptr;
    std::size_t ring = 0;

    std::size_t allowed() const noexcept { return limit ? std::min(len, *limit) : len; }
};

struct lane {
    std::vector<part> out, in;
    std::size_t oi = 0, ii = 0;
    clock::time_point last{};       // last byte either way: the stall clock
    clock::time_point last_rx{};    // last byte received, or when receiving became possible: who to blame
    bool was_active = false;
    bool was_rx = false;            // receiving was possible at the last look

    bool finished() const noexcept { return oi == out.size() && ii == in.size(); }
    bool can_send() const noexcept { return oi < out.size() && out[oi].allowed() > out[oi].done; }
    bool can_recv() const noexcept { return ii < in.size() && in[ii].allowed() > in[ii].done; }
};

inline void set_no_sigpipe(int fd)
{
#if defined(SO_NOSIGPIPE)
    const int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#else
    (void)fd;
#endif
}

inline ssize_t send_some(int fd, const char* p, std::size_t n)
{
#if defined(MSG_NOSIGNAL)
    return ::send(fd, p, n, MSG_NOSIGNAL);
#else
    return ::send(fd, p, n, 0);
#endif
}

// The reduction's element types: what is summed (always float) and what is sent. bfloat16 is
// snicholls-core's (ml/minifloat.hpp): decoding is exact, encoding rounds to nearest even.
inline float widen(float x) noexcept { return x; }
inline float widen(std::uint16_t bits) noexcept { return ml::to_float(ml::bfloat16::from_bits(bits)); }
inline void narrow(float x, float& out) noexcept { out = x; }
inline void narrow(float x, std::uint16_t& out) noexcept { out = ml::to_bfloat16(x).bits; }

} // namespace collective
} // namespace detail

/// One rank's end of a full TCP mesh, and the collectives over it. Movable,
/// not copyable. See the header comment for the semantics.
class collective {
public:
    collective() = default;
    collective(collective&&) noexcept = default;
    collective& operator=(collective&&) noexcept = default;
    collective(const collective&) = delete;
    collective& operator=(const collective&) = delete;

    /// Build the mesh: listen at peers[rank] (every rank but the last), dial
    /// every lower rank with retries, check every handshake. Bounded by
    /// cfg.connect_timeout.
    static collective_result<collective> connect(const collective_config& cfg)
    {
        return connect_impl(cfg, nullptr);
    }

    /// The same, with this rank's listener already bound (a test binds port 0
    /// and hands the ports round). Ignored on the last rank, which only dials,
    /// unless cfg.elastic.
    static collective_result<collective> connect(const collective_config& cfg, tcp_listener listener)
    {
        return connect_impl(cfg, &listener);
    }

    /// Elastic: join a running group as slot cfg.rank (a restarted rank). Dials
    /// the other slots in order for the group's rendezvous (its rank 0), is held
    /// there until the group's next admit(), then builds the mesh of the new
    /// epoch with every member. Bounded by cfg.join_timeout. A rendezvous of
    /// another run id refuses it (handshake_rejected).
    static collective_result<collective> join(const collective_config& cfg)
    {
        return join_impl(cfg, nullptr);
    }

    /// The same over a listener already bound at this slot's endpoint.
    static collective_result<collective> join(const collective_config& cfg, tcp_listener listener)
    {
        return join_impl(cfg, &listener);
    }

    /// This rank's place in the current membership, 0 <= rank() < size(): its
    /// slot's position among the members' slots in ascending order.
    int rank() const noexcept { return rank_; }
    int size() const noexcept { return size_; }

    /// This rank's slot: its fixed index in cfg.peers (cfg.rank), which re-formation never changes.
    int slot() const noexcept { return int(slot_); }

    /// The membership's number: 0 at connect(), one more at each re-formation or admission.
    std::uint64_t epoch() const noexcept { return epoch_; }

    /// The members' slots in rank order.
    const std::vector<std::uint32_t>& members() const noexcept { return members_; }

    /// The endpoint of a rank of the current membership, as configured.
    const std::string& endpoint(int r) const { return endpoints_.at(members_.at(std::size_t(r))); }

    /// True after a failure: every call then returns collective_errc::broken.
    bool broken() const noexcept { return broken_.has_value(); }

    /// The failure that broke the collective, if it is broken.
    const std::optional<collective_error>& failure() const noexcept { return broken_; }

    /// Collectives completed in this epoch (each one's sequence number is
    /// checked against every peer's); 0 again after a re-formation.
    std::uint64_t sequence() const noexcept { return seq_; }

    /// In place: every rank ends with, per element, ((x_0 + x_1) + ...) + x_{N-1}
    /// in rank order, in float. Bit-identical on every rank.
    collective_result<void> allreduce_sum(float* data, std::size_t n)
    {
        return allreduce(data, n, detail::collective::dtype_f32, rings_);
    }

    /// In place over bfloat16 bits (snicholls-core's ml::bfloat16::bits): every
    /// rank ends with, per element, bf16(((f(x_0) + f(x_1)) + ...) + f(x_{N-1})),
    /// where f decodes a bfloat16 to float exactly and the sum is in float in
    /// rank order (core's ml::ordered_sum_bf16), rounded once to bfloat16 to
    /// nearest even (core's ml::to_bfloat16) by the rank that owns the chunk.
    /// Bit-identical on every rank. Half the bytes of allreduce_sum on the wire:
    /// both the pieces and the reduced chunks travel as bfloat16.
    collective_result<void> allreduce_sum_bf16(std::uint16_t* data, std::size_t n)
    {
        return allreduce(data, n, detail::collective::dtype_bf16, rings16_);
    }

    /// Elastic, between collectives: a look without blocking for a failure the
    /// collective has not run into yet (an abort notice, a peer's connection
    /// gone, a survivor asking to re-form), and the rank's share of joining:
    /// the rendezvous (rank 0) takes joiners' requests and holds them for the
    /// next admit(); other ranks turn them away. Call it often between
    /// collectives (every training step): a group re-forming waits for every
    /// survivor's next poll(). An error means: reform().
    collective_result<void> poll()
    {
        if (broken_)
            return err(collective_error(collective_errc::broken, broken_->peer(), broken_->what()));
        if (!elastic_ || size_ <= 1) {
            if (elastic_)
                serve_listener();
            return ok();
        }
        serve_listener();
        if (reform_asked_)
            return finish(err(collective_error(collective_errc::peer_closed, -1,
                                               "a member is re-forming the group (a failure this rank has not seen)")));
        for (int q = 0; q < size_; ++q) {
            if (q == rank_)
                continue;
            const std::size_t i = std::size_t(q);
            if (ctrl_open_[i]) {
                if (auto e = read_notice(q); e.is_err())
                    return finish(std::move(e));
                if (!ctrl_open_[i])
                    return finish(fail(collective_errc::peer_closed, q, "its control connection closed"));
            }
            char c = 0;
            const ssize_t got = ::recv(links_[i].fd(), &c, 1, MSG_PEEK | MSG_DONTWAIT);
            if (got == 0)
                return finish(fail(collective_errc::peer_closed, q, "connection closed between collectives"));
            if (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                return finish(fail(errno == ECONNRESET ? collective_errc::peer_closed : collective_errc::io_error, q,
                                   std::string("connection lost: ") + std::strerror(errno)));
        }
        return ok();
    }

    /// Elastic, collective, at a round boundary: every member calls it. Rank 0
    /// says which joiners it holds; with none, nothing changes (one small
    /// broadcast). With some, the group moves to a new epoch whose members are
    /// the old ones and the joiners in slot order, and every member and joiner
    /// builds its mesh. Returns how many were admitted. A failure (also while
    /// building the new mesh) breaks the collective: reform().
    collective_result<std::size_t> admit()
    {
        namespace d = detail::collective;
        if (!elastic_)
            return err(collective_error(collective_errc::bad_config, -1, "admit() needs an elastic collective"));
        if (auto e = precheck(nullptr, 0, 1); e.is_err())
            return err(e.unwrap_err_unchecked());
        std::vector<float> decision(endpoints_.size() + 1, 0.0f);   // the count, then the slots
        std::vector<std::uint32_t> joiners;
        if (rank_ == 0) {
            serve_listener();
            for (auto& h : held_)
                if (h.kind == d::join_kind && h.s.valid() && !is_member(h.slot) &&
                    std::find(joiners.begin(), joiners.end(), h.slot) == joiners.end()) {
                    char c = 0;   // still there: a joiner that gave up has closed
                    const ssize_t got = ::recv(h.s.fd(), &c, 1, MSG_PEEK | MSG_DONTWAIT);
                    if (got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                        h.s.close();
                        continue;
                    }
                    joiners.push_back(h.slot);
                }
            decision[0] = float(joiners.size());
            for (std::size_t k = 0; k < joiners.size(); ++k)
                decision[k + 1] = float(joiners[k]);
        }
        if (size_ > 1)
            if (auto e = broadcast(decision.data(), decision.size(), 0); e.is_err())
                return err(e.unwrap_err_unchecked());
        if (size_ == 1)
            ++seq_;
        const std::size_t count = std::size_t(decision[0]);
        if (count == 0)
            return ok(std::size_t{0});
        joiners.clear();
        for (std::size_t k = 0; k < count; ++k)
            joiners.push_back(std::uint32_t(decision[k + 1]));
        std::vector<std::uint32_t> next = members_;
        next.insert(next.end(), joiners.begin(), joiners.end());
        std::sort(next.begin(), next.end());
        const std::uint64_t epoch = epoch_ + 1;
        if (rank_ == 0)
            for (auto& h : held_)
                if (h.kind == d::join_kind && h.s.valid() &&
                    std::find(joiners.begin(), joiners.end(), h.slot) != joiners.end()) {
                    send_view(h.s, epoch, next);
                    h.s.close();
                }
        adopt(epoch, next);
        const auto dl = d::clock::now() + connect_timeout_;
        if (auto e = build_mesh(dl); e.is_err()) {
            broken_ = e.unwrap_err_unchecked();
            return err(e.unwrap_err_unchecked());
        }
        return ok(count);
    }

    /// Elastic: after a failure (or a failed poll()), form a new collective
    /// among the survivors. Every survivor calls it. The coordinator is the
    /// lowest slot of the old membership that answers: each survivor dials the
    /// lower slots in order and registers with the first that does, and the
    /// one that finds none below it is the coordinator. It waits until every
    /// old member has registered or been blamed by a registered one (or
    /// gather_window passes), then sends every registered survivor the new
    /// membership - the survivors in slot order, epoch one more - and they
    /// build its mesh, whose handshake checks the epoch and the members. On
    /// success rank() and size() are the new ones and sequence() is 0. A rank
    /// the group re-formed without gets excluded: it must join(). Bounded by
    /// reform_timeout; on any other error it may be called again.
    collective_result<void> reform()
    {
        namespace d = detail::collective;
        using d::clock;
        if (!elastic_)
            return err(collective_error(collective_errc::bad_config, -1, "reform() needs an elastic collective"));
        const auto dl = clock::now() + reform_timeout_;
        const std::uint32_t blamed =
            broken_ && broken_->peer() >= 0 && broken_->peer() < size_ ? members_[std::size_t(broken_->peer())] : d::nobody;
        const std::vector<std::uint32_t> old = members_;
        const std::uint64_t e = epoch_, od = digest_;
        drop_mesh();
        broken_ = collective_error(collective_errc::broken, -1, "re-forming the group");

        // 1. Register with the lowest old member below this one that answers
        for (const std::uint32_t c : old) {
            if (c >= slot_)
                break;
            if (left_until(dl).count() == 0)
                return err(collective_error(collective_errc::timed_out, -1, "re-forming: no coordinator within reform_timeout"));
            auto s = tcp_stream::connect(eps_[c].host, eps_[c].port, std::min(probe_timeout_, left_until(dl)));
            if (!s.valid())
                continue;
            d::hello h;
            h.kind = d::register_kind;
            h.world = std::uint32_t(old.size());
            h.from = slot_;
            h.to = blamed;
            h.run_id = run_id_;
            h.epoch = e;
            h.digest = od;
            const auto b = d::encode(h);
            if (!s.write_all(reinterpret_cast<const char*>(b.data()), b.size(), std::max(left_until(dl), std::chrono::milliseconds(1))))
                continue;
            std::vector<std::uint32_t> view;
            std::uint64_t epoch = 0;
            unsigned reason = 0;
            int got = read_view(s, dl, view, epoch, reason);
            while (got == 2)
                got = read_view(s, dl, view, epoch, reason);
            if (got == 0)
                continue;                                   // gone or silent: the next one down
            if (got < 0) {
                if (reason == d::not_coordinator)
                    continue;
                return err(collective_error(reason == d::stale_epoch || reason == d::too_few ? collective_errc::excluded
                                                                                             : collective_errc::handshake_rejected,
                                            int(c), "slot " + std::to_string(c) + " (" + endpoints_[c] + "): " + d::reason_text(reason)));
            }
            if (std::find(view.begin(), view.end(), slot_) == view.end())
                return err(collective_error(collective_errc::excluded, int(c), "the new membership leaves this rank out"));
            adopt(epoch, view);
            return settle_mesh(build_mesh(std::min(dl, clock::now() + connect_timeout_)));
        }

        // 2. None below answered: this rank coordinates
        struct reg {
            std::uint32_t slot;
            tcp_stream s;
        };
        std::vector<reg> regs;
        std::vector<std::uint32_t> dead;
        if (blamed != d::nobody)
            dead.push_back(blamed);
        auto take = [&](tcp_stream s, const d::hello& h) {
            if (h.kind != d::register_kind) {
                serve(std::move(s), h);
                return;
            }
            if (!h.magic_ok || h.run_id != run_id_)
                return reject(s, h, d::bad_run_id);
            if (h.epoch < e)
                return reject(s, h, d::stale_epoch);
            if (h.epoch > e || h.digest != od || std::find(old.begin(), old.end(), h.from) == old.end() || h.from == slot_)
                return reject(s, h, d::not_coordinator);
            for (auto& r : regs)
                if (r.slot == h.from) {
                    r.s = std::move(s);                     // a second try: the newer connection
                    return;
                }
            if (h.to != d::nobody && h.to != slot_)
                dead.push_back(h.to);
            regs.push_back({h.from, std::move(s)});
        };
        auto pending = std::move(held_);
        held_.clear();
        for (auto& h : pending) {
            if (!h.s.valid())
                continue;
            if (h.kind == d::register_kind)
                take(std::move(h.s), h.h);
            else
                held_.push_back(std::move(h));
        }
        auto complete = [&] {
            for (const std::uint32_t m : old) {
                if (m == slot_ || std::find(dead.begin(), dead.end(), m) != dead.end())
                    continue;
                bool in = false;
                for (const auto& r : regs)
                    in = in || r.slot == m;
                if (!in)
                    return false;
            }
            return true;
        };
        const auto window = std::min(dl, clock::now() + gather_window_);
        while (!complete() && clock::now() < window) {
            auto s = listener_.accept(left_until(window));
            if (!s.valid())
                continue;
            d::hello h;
            if (!read_hello(s, std::min(window, clock::now() + handshake_timeout_), h))
                continue;
            take(std::move(s), h);
        }
        if (regs.size() + 1 < min_members_) {
            for (auto& r : regs)
                reject_slot(r.s, r.slot, d::too_few);
            return err(collective_error(collective_errc::excluded, -1,
                                        std::to_string(regs.size() + 1) + " survivors, fewer than min_members " + std::to_string(min_members_)));
        }
        std::vector<std::uint32_t> next{slot_};
        for (const auto& r : regs)
            next.push_back(r.slot);
        std::sort(next.begin(), next.end());
        for (auto& r : regs) {
            send_view(r.s, e + 1, next);
            r.s.close();
        }
        adopt(e + 1, next);
        return settle_mesh(build_mesh(std::min(dl, clock::now() + connect_timeout_)));
    }

    /// In place: every rank ends with root's n floats. The root sends the
    /// buffer once, in N-1 pieces; each other rank forwards its piece.
    /// (In an elastic group the root is a rank of the current membership.)
    collective_result<void> broadcast(float* data, std::size_t n, int root)
    {
        if (root < 0 || root >= size_)
            return err(collective_error(collective_errc::bad_config, -1,
                                        "broadcast root " + std::to_string(root) + " is not a rank"));
        if (auto e = precheck(data, n, sizeof(float)); e.is_err())
            return e;
        namespace d = detail::collective;
        const std::size_t N = std::size_t(size_), r = std::size_t(rank_), R = std::size_t(root);
        if (N == 1) {
            ++seq_;
            return ok();
        }
        auto lanes = start(d::op_broadcast, d::dtype_f32, std::uint32_t(root), n);

        // Pieces are numbered over the non-root ranks: rank q holds piece j(q)
        const std::size_t M = N - 1;
        auto j = [R](std::size_t q) { return q < R ? q : q - 1; };
        char* bytes = reinterpret_cast<char*>(data);
        auto piece = [&](std::size_t q) {
            return d::part{bytes + chunk_begin(n, M, j(q)) * sizeof(float), chunk_len(n, M, j(q)) * sizeof(float)};
        };
        for (std::size_t p = 0; p < N; ++p) {
            if (p == r)
                continue;
            if (r == R)
                lanes[p].out.push_back(piece(p));
            else if (p == R)
                lanes[p].in.push_back(piece(r));
            else {
                lanes[p].out.push_back(piece(r));                // forwarded as it arrives: gated below
                lanes[p].in.push_back(piece(p));
            }
        }
        if (r != R)
            for (std::size_t p = 0; p < N; ++p)
                if (p != r && p != R)
                    lanes[p].out[1].limit = &lanes[R].in[1].done;
        return finish(run(lanes, [] { return true; }));
    }

    /// Returns once every rank has entered the barrier.
    collective_result<void> barrier()
    {
        if (auto e = precheck(nullptr, 0, 1); e.is_err())
            return e;
        if (size_ == 1) {
            ++seq_;
            return ok();
        }
        auto lanes = start(detail::collective::op_barrier, detail::collective::dtype_none, 0, 0);
        return finish(run(lanes, [] { return true; }));
    }

    /// Element range of chunk c when n elements are cut into parts chunks: the
    /// first n % parts chunks are one longer. Public so a caller can tell which
    /// rank reduces which elements.
    static constexpr std::size_t chunk_begin(std::size_t n, std::size_t parts, std::size_t c) noexcept
    {
        return c * (n / parts) + std::min(c, n % parts);
    }
    static constexpr std::size_t chunk_len(std::size_t n, std::size_t parts, std::size_t c) noexcept
    {
        return n / parts + (c < n % parts ? 1 : 0);
    }

private:
    using lanes_t = std::vector<detail::collective::lane>;

    // A request held on the listener: a joiner waiting for admit(), or a survivor's registration
    struct held_request {
        tcp_stream s;
        std::uint32_t slot = 0;
        unsigned kind = 0;
        detail::collective::hello h;
    };

    int rank_ = 0;
    int size_ = 1;
    std::uint32_t slot_ = 0;
    std::vector<std::uint32_t> members_;             // slots in rank order
    std::uint64_t epoch_ = 0, digest_ = 0, run_id_ = 0;
    bool elastic_ = false;
    bool reform_asked_ = false;                      // a survivor registered with this rank: the group is failing
    tcp_listener listener_;                          // elastic: kept for re-formation and joiners
    std::vector<held_request> held_;
    std::vector<detail::collective::endpoint> eps_;
    std::chrono::milliseconds connect_timeout_{60'000}, handshake_timeout_{5'000}, probe_timeout_{2'000},
        gather_window_{120'000}, reform_timeout_{300'000};
    std::size_t min_members_ = 1;
    int socket_buffer_bytes_ = 0;
    std::vector<std::string> endpoints_;
    std::vector<tcp_stream> links_;                  // links_[p]: the data connection to rank p (none to itself)
    std::vector<tcp_stream> ctrl_;                   // ctrl_[p]: the control connection, for abort notices
    std::vector<std::array<unsigned char, detail::collective::abort_size>> notice_;   // a notice arriving
    std::vector<std::size_t> notice_got_;
    std::vector<char> ctrl_open_;                    // still worth reading
    bool adopted_ = false;                           // the failure in hand came from another rank's notice
    std::vector<char> heard_;                        // a progress notice arrived from this rank
    bool moved_ = false;                             // bytes moved since the last progress notice
    detail::collective::clock::time_point noted_{};  // when this rank last sent its progress notices
    std::chrono::milliseconds io_timeout_{30'000};
    std::size_t segment_bytes_ = std::size_t{1} << 20;
    std::uint64_t seq_ = 0;
    std::optional<collective_error> broken_;
    std::array<unsigned char, detail::collective::op_header_size> header_out_{};
    std::vector<std::array<unsigned char, detail::collective::op_header_size>> header_in_;
    std::vector<std::vector<float>> rings_;
    std::vector<std::vector<std::uint16_t>> rings16_;
    std::vector<float> acc_;

    std::string who(int p) const
    {
        const std::uint32_t s = p >= 0 && std::size_t(p) < members_.size() ? members_[std::size_t(p)] : std::uint32_t(p);
        const std::string& ep = s < endpoints_.size() ? endpoints_[s] : endpoints_.at(0);
        return "rank " + std::to_string(p) + (s == std::uint32_t(p) ? "" : " (slot " + std::to_string(s) + ")") + " (" + ep + ")";
    }

    bool is_member(std::uint32_t s) const { return std::find(members_.begin(), members_.end(), s) != members_.end(); }

    collective_result<void> precheck(const void* data, std::size_t n, std::size_t elem)
    {
        if (broken_)
            return err(collective_error(collective_errc::broken, broken_->peer(), broken_->what()));
        if (size_ < 1 || std::size_t(size_) != links_.size())
            return err(collective_error(collective_errc::bad_config, -1, "not connected"));
        if (n != 0 && data == nullptr)
            return err(collective_error(collective_errc::bad_config, -1, "null buffer"));
        if (n > std::numeric_limits<std::size_t>::max() / elem)
            return err(collective_error(collective_errc::bad_config, -1, "buffer too large"));
        if (reform_asked_)
            return finish(err(collective_error(collective_errc::peer_closed, -1,
                                               "a member is re-forming the group (a failure this rank has not seen)")));
        return ok();
    }

    // The all-reduce over float or bfloat16 elements T: pieces of T sent to the chunk's owner, which
    // decodes them to float, sums in rank order in float and encodes its chunk back to T in place,
    // then sends the reduced chunk of T to every other rank.
    template <class T>
    collective_result<void> allreduce(T* data, std::size_t n, unsigned dtype, std::vector<std::vector<T>>& rings)
    {
        namespace d = detail::collective;
        constexpr std::size_t E = sizeof(T);
        if (auto e = precheck(data, n, E); e.is_err())
            return e;
        const std::size_t N = std::size_t(size_), r = std::size_t(rank_);
        if (N == 1) {
            for (std::size_t i = 0; i < n; ++i)   // a bfloat16 NaN is quieted, as the reduction would
                d::narrow(d::widen(data[i]), data[i]);
            ++seq_;
            return ok();
        }
        auto lanes = start(d::op_allreduce_sum, dtype, 0, n);

        const std::size_t seg = segment_bytes_;                  // a multiple of 4, so of E
        const std::size_t K = 2;                                // segments of ring per peer
        const std::size_t mine = chunk_len(n, N, r) * E;
        const std::size_t nseg = (mine + seg - 1) / seg;
        std::size_t reduced = 0;                                 // segments reduced
        std::size_t reduced_bytes = 0;
        std::size_t ring_limit = std::min(mine, K * seg);
        rings.resize(N);
        for (std::size_t p = 0; p < N; ++p)
            if (p != r)
                rings[p].resize(K * seg / E);
        acc_.resize(seg / E);

        char* bytes = reinterpret_cast<char*>(data);
        for (std::size_t p = 0; p < N; ++p) {
            if (p == r)
                continue;
            auto& l = lanes[p];
            l.out.push_back({bytes + chunk_begin(n, N, p) * E, chunk_len(n, N, p) * E});
            l.out.push_back({bytes + chunk_begin(n, N, r) * E, mine, 0, &reduced_bytes});
            l.in.push_back({reinterpret_cast<char*>(rings[p].data()), mine, 0, &ring_limit, K * seg});
            l.in.push_back({bytes + chunk_begin(n, N, p) * E, chunk_len(n, N, p) * E});
        }

        T* own = data + chunk_begin(n, N, r);
        auto reduce = [&]() -> bool {
            while (reduced < nseg) {
                const std::size_t end = std::min(mine, (reduced + 1) * seg);
                for (std::size_t p = 0; p < N; ++p)
                    if (p != r && lanes[p].in[1].done < end)
                        return false;
                const std::size_t first = reduced * seg / E;
                const std::size_t count = (end - reduced * seg) / E;
                const std::size_t slot = (reduced % K) * seg / E;
                auto src = [&](std::size_t k) -> const T* {
                    return k == r ? own + first : rings[k].data() + slot;
                };
                // Rank order, one rank per pass, in float: acc = x0 + x1, then acc += x2, ...
                const T* a = src(0);
                const T* b = src(1);
                float* acc = acc_.data();
                for (std::size_t i = 0; i < count; ++i)
                    acc[i] = d::widen(a[i]) + d::widen(b[i]);
                for (std::size_t k = 2; k < N; ++k) {
                    const T* x = src(k);
                    for (std::size_t i = 0; i < count; ++i)
                        acc[i] = acc[i] + d::widen(x[i]);
                }
                for (std::size_t i = 0; i < count; ++i)
                    d::narrow(acc[i], own[first + i]);
                ++reduced;
                reduced_bytes = end;
                ring_limit = std::min(mine, (reduced + K) * seg);
            }
            return true;
        };
        return finish(run(lanes, reduce));
    }

    // Every lane starts by sending this rank's header and receiving the peer's
    lanes_t start(unsigned op, unsigned dtype, std::uint32_t root, std::size_t n)
    {
        namespace d = detail::collective;
        header_out_ = d::encode(d::op_header{op, dtype, root, std::uint32_t(size_), seq_, std::uint64_t(n), epoch_});
        header_in_.assign(std::size_t(size_), {});
        lanes_t lanes(static_cast<std::size_t>(size_));
        for (std::size_t p = 0; p < lanes.size(); ++p) {
            if (int(p) == rank_)
                continue;
            lanes[p].out.reserve(3);
            lanes[p].in.reserve(3);
            lanes[p].out.push_back({reinterpret_cast<char*>(header_out_.data()), header_out_.size()});
            lanes[p].in.push_back({reinterpret_cast<char*>(header_in_[p].data()), d::op_header_size});
        }
        return lanes;
    }

    // A failure is told to every other rank on the control connections before anything closes
    // (unless it was itself told by one), so they fail at once and blame the same rank.
    collective_result<void> finish(collective_result<void> r)
    {
        namespace d = detail::collective;
        if (r.is_ok()) {
            ++seq_;
            return r;
        }
        broken_ = r.unwrap_err_unchecked();
        if (!adopted_) {
            std::array<unsigned char, d::abort_size> note{};
            note[0] = static_cast<unsigned char>(d::abort_magic & 0xff);
            note[1] = static_cast<unsigned char>(d::abort_magic >> 8);
            note[2] = static_cast<unsigned char>(broken_->code());
            d::put32(note.data() + 4, broken_->peer() < 0 ? d::nobody : std::uint32_t(broken_->peer()));
            for (std::size_t q = 0; q < ctrl_.size(); ++q)
                if (ctrl_[q].valid())
                    (void)::send(ctrl_[q].fd(), note.data(), note.size(), MSG_DONTWAIT
#if defined(MSG_NOSIGNAL)
                                                                              | MSG_NOSIGNAL
#endif
                    );
        }
        adopted_ = false;
        return r;
    }

    // Read what rank q's control connection has. Returns an error when a whole abort notice has
    // arrived, ok otherwise; a closed control connection is marked and left alone (the data
    // connection says whether that was a failure).
    collective_result<void> read_notice(int q)
    {
        namespace d = detail::collective;
        const std::size_t i = std::size_t(q);
        for (;;) {
            while (ctrl_open_[i] && notice_got_[i] < d::abort_size) {
                const ssize_t got = ::recv(ctrl_[i].fd(), notice_[i].data() + notice_got_[i],
                                           d::abort_size - notice_got_[i], MSG_DONTWAIT);
                if (got > 0) {
                    notice_got_[i] += std::size_t(got);
                    continue;
                }
                if (got < 0 && errno == EINTR)
                    continue;
                if (got == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
                    ctrl_open_[i] = 0;
                return ok();
            }
            if (notice_got_[i] < d::abort_size)
                return ok();
            if ((unsigned(notice_[i][0]) | unsigned(notice_[i][1]) << 8) != d::progress_magic)
                break;
            notice_got_[i] = 0;                          // progress: note it, read on
            heard_[i] = 1;
        }
        const auto& b = notice_[i];
        const unsigned code = b[2];
        const std::uint32_t culprit = d::get32(b.data() + 4);
        if ((unsigned(b[0]) | unsigned(b[1]) << 8) != d::abort_magic || code < 1 ||
            code > unsigned(collective_errc::excluded)) {
            ctrl_open_[i] = 0;
            return fail(collective_errc::io_error, q, "garbage on the control connection");
        }
        adopted_ = true;
        const auto c = static_cast<collective_errc>(code);
        if (culprit == std::uint32_t(rank_))
            return err(collective_error(c, q, who(q) + " gave up on this rank: " + to_string(c)));
        if (culprit == d::nobody || culprit >= std::uint32_t(size_))
            return err(collective_error(c, -1, who(q) + " failed: " + to_string(c)));
        return err(collective_error(c, int(culprit), who(int(culprit)) + ": found by " + who(q) +
                                                         ", which told every rank"));
    }

    collective_result<void> fail(collective_errc code, int p, const std::string& detail) const
    {
        return err(collective_error(code, p, who(p) + ": " + detail));
    }

    // The loop every collective runs: poll every peer with work, move what may
    // move, let `step` (the reduction) advance the gates, until every part of
    // every lane is done and `step` says it is finished.
    template <class Step>
    collective_result<void> run(lanes_t& lanes, Step&& step)
    {
        namespace d = detail::collective;
        using d::clock;
        const auto t0 = clock::now();
        for (auto& l : lanes)
            l.last = l.last_rx = t0;
        std::vector<pollfd> pfds;
        std::vector<std::size_t> owner;
        auto checked = t0;
        pfds.reserve(lanes.size());
        owner.reserve(lanes.size());

        for (;;) {
            const bool stepped = step();
            bool all = stepped;
            pfds.clear();
            owner.clear();
            const auto now = clock::now();
            int wait_ms = -1;
            for (std::size_t p = 0; p < lanes.size(); ++p) {
                auto& l = lanes[p];
                if (int(p) == rank_ || l.finished())
                    continue;
                all = false;
                short ev = 0;
                if (l.can_send())
                    ev |= POLLOUT;
                if (l.can_recv())
                    ev |= POLLIN;
                const bool active = ev != 0;
                const bool rx = (ev & POLLIN) != 0;
                if (active && !l.was_active)
                    l.last = now;                       // the clocks start when work becomes possible
                if (rx && !l.was_rx)
                    l.last_rx = now;
                l.was_active = active;
                l.was_rx = rx;
                if (active) {
                    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(l.last + io_timeout_ - now);
                    const int ms = left.count() <= 0 ? 0 : left.count() > 0x7fffffff ? 0x7fffffff : int(left.count());
                    wait_ms = wait_ms < 0 ? ms : std::min(wait_ms, ms);
                }
                pfds.push_back({links_[p].fd(), ev, 0});
                owner.push_back(p);
            }
            if (all)
                return ok();
            const std::size_t data_fds = pfds.size();
            for (std::size_t q = 0; q < ctrl_.size(); ++q)
                if (int(q) != rank_ && ctrl_open_[q]) {
                    pfds.push_back({ctrl_[q].fd(), POLLIN, 0});
                    owner.push_back(q);
                }
            if (wait_ms < 0)
                return err(collective_error(collective_errc::io_error, -1,
                                            "internal: no transfer can proceed (a bug in net/collective.hpp)"));

            // A peer this rank is not reading from just now (its ring is full, its next part not yet
            // sendable) can still die. Its reset shows as a pending socket error: look every 20 ms,
            // so a death is seen at once, not first through another rank giving up because of it.
            // A FIN is not an error here - a peer that has everything it needs may finish and close
            // while its last bytes still wait unread in this rank's buffer.
            bool idle_lanes = false;
            for (std::size_t i = 0; i < data_fds; ++i)
                idle_lanes = idle_lanes || pfds[i].events == 0;
            if (idle_lanes) {
                wait_ms = std::min(wait_ms, 20);
                if (now - checked >= std::chrono::milliseconds(20)) {
                    checked = now;
                    for (std::size_t i = 0; i < data_fds; ++i) {
                        if (pfds[i].events != 0)
                            continue;
                        int se = 0;
                        socklen_t sl = sizeof se;
                        if (::getsockopt(pfds[i].fd, SOL_SOCKET, SO_ERROR, &se, &sl) == 0 && se != 0)
                            return settle(lanes, int(owner[i]),
                                          fail(se == ECONNRESET || se == EPIPE ? collective_errc::peer_closed
                                                                               : collective_errc::io_error,
                                               int(owner[i]), std::string("connection lost: ") + std::strerror(se)));
                    }
                }
            }

            const int rc = ::poll(pfds.data(), nfds_t(pfds.size()), wait_ms);
            if (rc < 0 && errno != EINTR)
                return err(collective_error(collective_errc::io_error, -1, std::string("poll: ") + std::strerror(errno)));

            // Another rank's failure first: it decides what this one reports
            for (std::size_t i = data_fds; rc > 0 && i < pfds.size(); ++i)
                if (pfds[i].revents != 0)
                    if (auto e = read_notice(int(owner[i])); e.is_err())
                        return e;
            // A rank that says it is moving bytes is not stalled, whatever it owes this one
            for (std::size_t q = 0; q < heard_.size(); ++q)
                if (heard_[q]) {
                    heard_[q] = 0;
                    lanes[q].last = lanes[q].last_rx = clock::now();
                }

            for (std::size_t i = 0; rc > 0 && i < data_fds; ++i) {
                const std::size_t p = owner[i];
                const short rev = pfds[i].revents;
                if (rev == 0)
                    continue;
                if (rev & (POLLIN | POLLHUP | POLLERR)) {
                    if (lanes[p].can_recv()) {
                        if (auto e = receive(lanes[p], int(p)); e.is_err())
                            return settle(lanes, int(p), std::move(e));
                    }
                }
                if (rev & (POLLOUT | POLLHUP | POLLERR)) {
                    if (lanes[p].can_send()) {
                        if (auto e = transmit(lanes[p], int(p)); e.is_err())
                            return settle(lanes, int(p), std::move(e));
                    }
                }
                // Hung up or failed with work still owed, and nothing above noticed: report it
                if ((rev & (POLLHUP | POLLERR | POLLNVAL)) && !lanes[p].finished() &&
                    !lanes[p].can_recv() && !lanes[p].can_send()) {
                    int se = 0;
                    socklen_t sl = sizeof se;
                    ::getsockopt(pfds[i].fd, SOL_SOCKET, SO_ERROR, &se, &sl);
                    return settle(lanes, int(p),
                                  fail(collective_errc::peer_closed, int(p),
                                       se ? std::string(std::strerror(se)) : "connection closed mid-operation"));
                }
            }

            // This rank's own progress, told to every other rank now and then (see progress_magic)
            if (moved_ && clock::now() - noted_ >= io_timeout_ / 4) {
                moved_ = false;
                noted_ = clock::now();
                std::array<unsigned char, d::abort_size> note{};
                note[0] = static_cast<unsigned char>(d::progress_magic & 0xff);
                note[1] = static_cast<unsigned char>(d::progress_magic >> 8);
                for (std::size_t q = 0; q < ctrl_.size(); ++q)
                    if (int(q) != rank_ && ctrl_[q].valid())
                        (void)::send(ctrl_[q].fd(), note.data(), note.size(), MSG_DONTWAIT
#if defined(MSG_NOSIGNAL)
                                                                                  | MSG_NOSIGNAL
#endif
                        );
            }

            // Stall check. When any peer with work possible has moved no byte for io_timeout, the
            // peer blamed is the one this rank has waited on to send the longest: a stall is
            // nearly always waiting for data, and peers held up by the same culprit have been
            // heard from more recently than it. With nothing to receive, the longest-stuck send.
            const auto later = clock::now();
            bool stalled = false;
            std::size_t worst = lanes.size();
            auto key = [&](std::size_t q) {
                return lanes[q].can_recv() ? std::pair{0, lanes[q].last_rx} : std::pair{1, lanes[q].last};
            };
            for (std::size_t p = 0; p < lanes.size(); ++p) {
                const auto& l = lanes[p];
                if (int(p) == rank_ || !l.was_active || !(l.can_send() || l.can_recv()))
                    continue;
                stalled = stalled || later - l.last >= io_timeout_;
                if (worst == lanes.size() || key(p) < key(worst))
                    worst = p;
            }
            if (stalled) {
                const auto& l = lanes[worst];
                const bool rx = l.can_recv();
                const auto& pt = rx ? l.in[l.ii] : l.out[l.oi];
                return fail(collective_errc::timed_out, int(worst),
                            "no progress for " + std::to_string(io_timeout_.count()) + " ms " +
                                (rx ? "receiving" : "sending") + " (" + std::to_string(pt.done) + " of " +
                                std::to_string(pt.len) + " bytes of part " + std::to_string(rx ? l.ii : l.oi) +
                                ", " + d::op_name(header_out_[4]) + ")");
            }
        }
    }

    // Rank p's data connection failed. Before blaming p, give its control connection a moment
    // (100 ms, or io_timeout if shorter): if p gave up because of another rank, its notice says
    // which, and arrives before its sockets close. A control connection that ends with no notice
    // means p itself went away.
    collective_result<void> settle(lanes_t&, int p, collective_result<void> first)
    {
        namespace d = detail::collective;
        const std::size_t i = std::size_t(p);
        const auto until = d::clock::now() + std::min(io_timeout_, std::chrono::milliseconds(100));
        while (i < ctrl_.size() && ctrl_open_[i]) {
            if (auto e = read_notice(p); e.is_err())
                return e;
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - d::clock::now());
            if (!ctrl_open_[i] || left.count() <= 0)
                break;
            pollfd pf{ctrl_[i].fd(), POLLIN, 0};
            (void)::poll(&pf, 1, int(left.count()));
        }
        return first;
    }

    collective_result<void> receive(detail::collective::lane& l, int p)
    {
        namespace d = detail::collective;
        const int fd = links_[std::size_t(p)].fd();
        while (l.can_recv()) {
            auto& pt = l.in[l.ii];
            std::size_t want = pt.allowed() - pt.done;
            std::size_t at = pt.done;
            if (pt.ring) {
                at = pt.done % pt.ring;
                want = std::min(want, pt.ring - at);
            }
            const ssize_t got = ::recv(fd, pt.base + at, want, 0);
            if (got == 0)
                return fail(collective_errc::peer_closed, p,
                            "connection closed with " + std::to_string(pt.len - pt.done) + " bytes still to come");
            if (got < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return ok();
                if (errno == EINTR)
                    continue;
                return fail(errno == ECONNRESET ? collective_errc::peer_closed : collective_errc::io_error, p,
                            std::string("recv: ") + std::strerror(errno));
            }
            pt.done += std::size_t(got);
            moved_ = true;
            l.last = l.last_rx = d::clock::now();
            if (pt.done == pt.len) {
                if (l.ii == 0 && std::memcmp(pt.base, header_out_.data(), d::op_header_size) != 0)
                    return fail(collective_errc::mismatched_call, p,
                                "it called " + d::describe(reinterpret_cast<const unsigned char*>(pt.base)) +
                                    ", this rank " + d::describe(header_out_.data()));
                ++l.ii;
                while (l.ii < l.in.size() && l.in[l.ii].len == 0)
                    ++l.ii;
            }
        }
        return ok();
    }

    collective_result<void> transmit(detail::collective::lane& l, int p)
    {
        namespace d = detail::collective;
        const int fd = links_[std::size_t(p)].fd();
        while (l.can_send()) {
            auto& pt = l.out[l.oi];
            const ssize_t sent = d::send_some(fd, pt.base + pt.done, pt.allowed() - pt.done);
            if (sent < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return ok();
                if (errno == EINTR)
                    continue;
                return fail(errno == EPIPE || errno == ECONNRESET ? collective_errc::peer_closed
                                                                  : collective_errc::io_error,
                            p, std::string("send: ") + std::strerror(errno));
            }
            pt.done += std::size_t(sent);
            moved_ = true;
            l.last = d::clock::now();
            if (pt.done == pt.len) {
                ++l.oi;
                while (l.oi < l.out.size() && l.out[l.oi].len == 0)
                    ++l.oi;
            }
        }
        return ok();
    }

    // ----------------------------------------------------------- connecting

    // Read exactly n bytes before the deadline: 1 done, 0 peer closed, -1 timed out, -2 error (in *e)
    static int read_exact(tcp_stream& s, unsigned char* buf, std::size_t n, detail::collective::clock::time_point dl,
                          std::string* e)
    {
        std::size_t got = 0;
        while (got < n) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(dl - detail::collective::clock::now());
            if (left.count() <= 0)
                return -1;
            const auto r = s.read(reinterpret_cast<char*>(buf) + got, n - got, left, e);
            if (r == 0)
                return 0;
            if (r < 0)
                return errno == ETIMEDOUT ? -1 : -2;
            got += std::size_t(r);
        }
        return 1;
    }

    static std::chrono::milliseconds left_until(detail::collective::clock::time_point dl)
    {
        const auto l = std::chrono::duration_cast<std::chrono::milliseconds>(dl - detail::collective::clock::now());
        return l.count() < 0 ? std::chrono::milliseconds(0) : l;
    }

    // Check the configuration and set up this rank's object, its listener bound; no connection yet
    static collective_result<collective> setup(const collective_config& cfg, tcp_listener* given, bool listen_always)
    {
        namespace d = detail::collective;
        auto bad = [](const std::string& s) { return err(collective_error(collective_errc::bad_config, -1, s)); };

        const std::size_t N = cfg.peers.size();
        if (N == 0 || N > 0xffff)
            return bad("peers must list between 1 and 65535 ranks");
        if (cfg.rank < 0 || std::size_t(cfg.rank) >= N)
            return bad("rank " + std::to_string(cfg.rank) + " is not in 0.." + std::to_string(N - 1));
        if (cfg.io_timeout.count() <= 0 || cfg.connect_timeout.count() <= 0 || cfg.handshake_timeout.count() <= 0)
            return bad("timeouts must be positive");
        if (cfg.elastic && (cfg.probe_timeout.count() <= 0 || cfg.gather_window.count() <= 0 || cfg.reform_timeout.count() <= 0 ||
                            cfg.join_timeout.count() <= 0))
            return bad("timeouts must be positive");
        if (cfg.segment_bytes < sizeof(float) || cfg.segment_bytes > (std::size_t{1} << 30))
            return bad("segment_bytes must be between 4 bytes and 1 GiB");

        collective c;
        for (const auto& s : cfg.peers) {
            auto e = d::parse_endpoint(s);
            if (!e)
                return bad("\"" + s + "\" is not host:port");
            c.eps_.push_back(*e);
        }
        c.slot_ = std::uint32_t(cfg.rank);
        c.run_id_ = cfg.run_id;
        c.elastic_ = cfg.elastic;
        c.endpoints_ = cfg.peers;
        c.io_timeout_ = cfg.io_timeout;
        c.connect_timeout_ = cfg.connect_timeout;
        c.handshake_timeout_ = cfg.handshake_timeout;
        c.probe_timeout_ = cfg.probe_timeout;
        c.gather_window_ = cfg.gather_window;
        c.reform_timeout_ = cfg.reform_timeout;
        c.min_members_ = cfg.min_members;
        c.socket_buffer_bytes_ = cfg.socket_buffer_bytes;
        c.segment_bytes_ = cfg.segment_bytes / sizeof(float) * sizeof(float);
        if (N > 1 && (listen_always || std::size_t(cfg.rank) + 1 < N)) {
            if (given && given->valid())
                c.listener_ = std::move(*given);
            else {
                std::string e;
                c.listener_ = tcp_listener::bind(cfg.listen_host, c.eps_[c.slot_].port, 128, &e);
                if (!c.listener_.valid())
                    return err(collective_error(collective_errc::bind_failed, cfg.rank,
                                                "slot " + std::to_string(cfg.rank) + " (" + cfg.peers[c.slot_] + "): " + e));
            }
        }
        return ok(std::move(c));
    }

    static collective_result<collective> connect_impl(const collective_config& cfg, tcp_listener* given)
    {
        auto made = setup(cfg, given, cfg.elastic);
        if (made.is_err())
            return made;
        collective c = std::move(made).unwrap();
        std::vector<std::uint32_t> all(cfg.peers.size());
        for (std::size_t i = 0; i < all.size(); ++i)
            all[i] = std::uint32_t(i);
        c.adopt(0, all);
        if (auto e = c.build_mesh(detail::collective::clock::now() + cfg.connect_timeout); e.is_err())
            return err(e.unwrap_err_unchecked());
        if (!c.elastic_)
            c.listener_.close();
        return ok(std::move(c));
    }

    // Take a membership as this rank's: its epoch, digest, rank and size, and a clean slate
    void adopt(std::uint64_t epoch, std::vector<std::uint32_t> slots)
    {
        members_ = std::move(slots);
        epoch_ = epoch;
        digest_ = detail::collective::view_digest(run_id_, epoch_, members_);
        size_ = int(members_.size());
        rank_ = int(std::find(members_.begin(), members_.end(), slot_) - members_.begin());
        seq_ = 0;
        adopted_ = false;
        reform_asked_ = false;
        drop_mesh();
        links_.resize(members_.size());
        ctrl_.resize(members_.size());
        notice_.assign(members_.size(), {});
        notice_got_.assign(members_.size(), 0);
        ctrl_open_.assign(members_.size(), 0);
        heard_.assign(members_.size(), 0);
    }

    void drop_mesh()
    {
        links_.clear();
        ctrl_.clear();
        ctrl_open_.assign(ctrl_open_.size(), 0);
    }

    collective_result<void> settle_mesh(collective_result<void> r)
    {
        if (r.is_err()) {
            broken_ = r.unwrap_err_unchecked();
            return r;
        }
        broken_.reset();
        return r;
    }

    // Read one frame before the deadline; false if it did not come whole
    static bool read_hello(tcp_stream& s, detail::collective::clock::time_point dl, detail::collective::hello& h)
    {
        std::array<unsigned char, detail::collective::hello_size> buf{};
        std::string e;
        if (read_exact(s, buf.data(), buf.size(), dl, &e) != 1)
            return false;
        h = detail::collective::decode(buf.data());
        return true;
    }

    detail::collective::hello frame(unsigned char kind, unsigned char reason, std::uint32_t to) const
    {
        detail::collective::hello h;
        h.kind = kind;
        h.reason = reason;
        h.world = std::uint32_t(members_.size());
        h.from = slot_;
        h.to = to;
        h.run_id = run_id_;
        h.epoch = epoch_;
        h.digest = digest_;
        return h;
    }

    void reject_slot(tcp_stream& s, std::uint32_t to, unsigned char why) const
    {
        const auto b = detail::collective::encode(frame(detail::collective::reject_kind, why, to));
        (void)s.write_all(reinterpret_cast<const char*>(b.data()), b.size(), std::chrono::milliseconds(200));
        s.close();
    }
    void reject(tcp_stream& s, const detail::collective::hello& h, unsigned char why) const { reject_slot(s, h.from, why); }

    // The new membership to a registered survivor or an admitted joiner: the frame, then the slots
    void send_view(tcp_stream& s, std::uint64_t epoch, const std::vector<std::uint32_t>& slots) const
    {
        namespace d = detail::collective;
        d::hello h = frame(d::view_kind, d::ok_reason, 0);
        h.epoch = epoch;
        h.world = std::uint32_t(slots.size());
        h.digest = d::view_digest(run_id_, epoch, slots);
        std::vector<unsigned char> b(d::hello_size + 4 * slots.size());
        const auto f = d::encode(h);
        std::memcpy(b.data(), f.data(), f.size());
        for (std::size_t k = 0; k < slots.size(); ++k)
            d::put32(b.data() + d::hello_size + 4 * k, slots[k]);
        (void)s.write_all(reinterpret_cast<const char*>(b.data()), b.size(), std::max(handshake_timeout_, std::chrono::milliseconds(1)));
    }

    // One answer to a register or join: 1 a view (in view, epoch), 2 wait (the view follows later),
    // -1 a refusal (in reason), 0 nothing usable (closed, timed out, garbage)
    int read_view(tcp_stream& s, detail::collective::clock::time_point dl, std::vector<std::uint32_t>& view, std::uint64_t& epoch,
                  unsigned& reason) const
    {
        namespace d = detail::collective;
        {
            d::hello h;
            if (!read_hello(s, dl, h) || !h.magic_ok)
                return 0;
            if (h.kind == d::wait_kind)
                return 2;
            if (h.kind == d::reject_kind) {
                reason = h.reason;
                return -1;
            }
            if (h.kind != d::view_kind || h.run_id != run_id_ || h.world == 0 || h.world > endpoints_.size())
                return 0;
            std::vector<unsigned char> b(4 * std::size_t(h.world));
            std::string e;
            if (read_exact(s, b.data(), b.size(), dl, &e) != 1)
                return 0;
            view.resize(h.world);
            for (std::size_t k = 0; k < view.size(); ++k)
                view[k] = d::get32(b.data() + 4 * k);
            if (!std::is_sorted(view.begin(), view.end()) || std::adjacent_find(view.begin(), view.end()) != view.end() ||
                view.back() >= endpoints_.size() || d::view_digest(run_id_, h.epoch, view) != h.digest)
                return 0;
            epoch = h.epoch;
            return 1;
        }
    }

    // A request on the listener outside a re-formation this rank coordinates: a joiner is held by the
    // rendezvous (rank 0) and turned away elsewhere; a registration from this epoch means the group is
    // failing (held for the coordinator this rank may become), one from an older epoch is told it is
    // out, one from a newer epoch is not this rank's to answer
    void serve(tcp_stream s, const detail::collective::hello& h)
    {
        namespace d = detail::collective;
        if (!h.magic_ok)
            return reject(s, h, d::bad_magic);
        if (h.run_id != run_id_)
            return reject(s, h, d::bad_run_id);
        if (h.from >= endpoints_.size() || h.from == slot_)
            return reject(s, h, d::bad_rank);
        if (h.kind == d::join_kind) {
            if (!elastic_ || rank_ != 0 || members_.empty())
                return reject(s, h, d::not_coordinator);
            if (is_member(h.from))
                return reject(s, h, d::duplicate_rank);
            const auto b = d::encode(frame(d::wait_kind, d::ok_reason, h.from));
            if (!s.write_all(reinterpret_cast<const char*>(b.data()), b.size(), std::chrono::milliseconds(200)))
                return;
            for (auto& q : held_)
                if (q.kind == d::join_kind && q.slot == h.from) {
                    q.s = std::move(s);
                    return;
                }
            held_.push_back({std::move(s), h.from, d::join_kind, h});
            return;
        }
        if (h.kind == d::register_kind && elastic_) {
            if (h.epoch < epoch_)
                return reject(s, h, d::stale_epoch);
            if (h.epoch > epoch_)
                return reject(s, h, d::not_coordinator);
            reform_asked_ = reform_asked_ || h.digest == digest_;
            held_.push_back({std::move(s), h.from, d::register_kind, h});
            return;
        }
        reject(s, h, d::bad_magic);
    }

    // Between collectives: every connection waiting on the listener, read and served. A frame must
    // come at once (they are sent on connecting): this never holds the caller up for long
    void serve_listener()
    {
        namespace d = detail::collective;
        if (!listener_.valid())
            return;
        for (int k = 0; k < 64; ++k) {
            pollfd pf{listener_.fd(), POLLIN, 0};
            if (::poll(&pf, 1, 0) <= 0)
                return;
            auto s = listener_.accept(std::chrono::milliseconds(0));
            if (!s.valid())
                return;
            d::hello h;
            if (read_hello(s, d::clock::now() + std::min(handshake_timeout_, std::chrono::milliseconds(200)), h))
                serve(std::move(s), h);
        }
    }

    // The full mesh of the current membership: dial every lower rank twice (data, control) and say
    // hello, accept every higher rank's two connections, collect the lower ranks' answers. Each
    // hello names the run, the epoch and the members (their digest), so two ranks holding
    // different memberships never connect. Requests that arrive meanwhile are served.
    collective_result<void> build_mesh(detail::collective::clock::time_point dl)
    {
        namespace d = detail::collective;
        using d::clock;
        const std::size_t N = members_.size();
        const std::size_t r = std::size_t(rank_);
        if (N <= 1)
            return ok();
        auto link = [this](std::size_t p, unsigned ch) -> tcp_stream& { return ch == 0 ? links_[p] : ctrl_[p]; };
        auto hi = [&](std::size_t to, unsigned char channel, unsigned char kind, unsigned char reason) {
            d::hello h = frame(kind, reason, to < N ? members_[to] : 0);
            h.channel = channel;
            return d::encode(h);
        };

        // 1. Dial every lower rank twice. Each of them listens before it dials anyone, so this waits
        //    only for processes to start, never on a cycle.
        const backoff_policy policy{std::chrono::milliseconds(20), std::chrono::milliseconds(500)};
        for (std::size_t p = 0; p < 2 * r; ++p) {
            const unsigned ch = unsigned(p % 2);
            const std::size_t to = p / 2;
            const auto& ep = eps_[members_[to]];
            std::string last;
            for (std::uint32_t attempt = 0;; ++attempt) {
                const auto left = left_until(dl);
                if (left.count() == 0)
                    return err(collective_error(collective_errc::connect_failed, int(to),
                                                who(int(to)) + ": not reachable within " +
                                                    std::to_string(connect_timeout_.count()) + " ms" +
                                                    (last.empty() ? "" : " (" + last + ")")));
                auto s = tcp_stream::connect(ep.host, ep.port, std::min(left, std::chrono::milliseconds(2000)), &last);
                if (s.valid()) {
                    link(to, ch) = std::move(s);
                    break;
                }
                std::this_thread::sleep_for(std::min(backoff_ceiling(policy, attempt), left_until(dl)));
            }
            const auto h = hi(to, static_cast<unsigned char>(ch), d::hello_kind, d::ok_reason);
            std::string e;
            if (!link(to, ch).write_all(reinterpret_cast<const char*>(h.data()), h.size(),
                                        std::max(left_until(dl), std::chrono::milliseconds(1)), &e))
                return err(collective_error(collective_errc::connect_failed, int(to), who(int(to)) + ": " + e));
        }

        // 2. Accept every higher rank: check its hello, answer it. A connection that is not one of
        //    this membership's ranks is refused (or served, if it is a request), and the wait goes on.
        std::string refused;
        int refused_rank = -1;
        for (std::size_t accepted = 0; accepted < 2 * (N - 1 - r);) {
            std::string e;
            auto s = listener_.accept(left_until(dl), &e);
            if (!s.valid()) {
                if (errno != ETIMEDOUT && left_until(dl).count() > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));   // EMFILE and friends
                    continue;
                }
                std::size_t missing = r + 1;
                while (missing < N && links_[missing].valid() && ctrl_[missing].valid())
                    ++missing;
                if (!refused.empty())
                    return err(collective_error(collective_errc::handshake_rejected, refused_rank,
                                                "waiting for " + who(int(missing)) + "; refused " + refused));
                return err(collective_error(collective_errc::timed_out, int(missing),
                                            who(int(missing)) + ": did not connect within " +
                                                std::to_string(connect_timeout_.count()) + " ms"));
            }
            d::hello h;
            if (!read_hello(s, std::min(dl, clock::now() + handshake_timeout_), h))
                continue;                                         // silent or gone: not one of ours
            if (h.magic_ok && (h.kind == d::join_kind || h.kind == d::register_kind)) {
                serve(std::move(s), h);
                continue;
            }
            const std::size_t from = std::size_t(std::find(members_.begin(), members_.end(), h.from) - members_.begin());
            unsigned char why = d::ok_reason;
            if (!h.magic_ok || h.kind != d::hello_kind || h.channel > 1)
                why = d::bad_magic;
            else if (!h.native_order)
                why = d::bad_byte_order;
            else if (h.world != N)
                why = d::bad_world;
            else if (h.to != slot_ || from <= r || from >= N)
                why = d::bad_rank;
            else if (h.run_id != run_id_)
                why = d::bad_run_id;
            else if (h.epoch != epoch_ || h.digest != digest_)
                why = d::bad_view;
            else if (link(from, h.channel).valid())
                why = d::duplicate_rank;
            d::hello reply = frame(why == d::ok_reason ? d::accept_kind : d::reject_kind, why, h.magic_ok ? h.from : 0);
            reply.channel = h.channel;
            const auto rb = d::encode(reply);
            const bool sent = s.write_all(reinterpret_cast<const char*>(rb.data()), rb.size(),
                                          std::max(left_until(std::min(dl, clock::now() + handshake_timeout_)), std::chrono::milliseconds(1)), &e);
            if (why != d::ok_reason) {
                refused_rank = h.magic_ok && from < N ? int(from) : -1;
                refused = "a connection claiming slot " + std::to_string(h.from) + ": " + d::reason_text(why);
                continue;
            }
            if (!sent)
                continue;
            link(from, h.channel) = std::move(s);
            ++accepted;
        }

        // 3. Collect the lower ranks' answers.
        for (std::size_t q = 0; q < 2 * r; ++q) {
            const std::size_t p = q / 2;
            const unsigned ch = unsigned(q % 2);
            std::array<unsigned char, d::hello_size> buf{};
            std::string e;
            const int got = read_exact(link(p, ch), buf.data(), buf.size(), dl, &e);
            if (got != 1)
                return err(collective_error(got == -1 ? collective_errc::timed_out : collective_errc::handshake_rejected,
                                            int(p),
                                            who(int(p)) + ": no handshake answer" +
                                                (got == 0 ? " (connection closed)" : e.empty() ? "" : " (" + e + ")")));
            const auto h = d::decode(buf.data());
            const char* why = nullptr;
            if (!h.magic_ok)
                why = d::reason_text(d::bad_magic);
            else if (h.kind == d::reject_kind)
                why = d::reason_text(h.reason);
            else if (h.kind != d::accept_kind || h.from != members_[p] || h.to != slot_ || h.channel != ch)
                why = d::reason_text(d::bad_rank);
            else if (h.world != N)
                why = d::reason_text(d::bad_world);
            else if (h.run_id != run_id_)
                why = d::reason_text(d::bad_run_id);
            else if (!h.native_order)
                why = d::reason_text(d::bad_byte_order);
            else if (h.epoch != epoch_ || h.digest != digest_)
                why = d::reason_text(d::bad_view);
            if (why)
                return err(collective_error(collective_errc::handshake_rejected, int(p), who(int(p)) + ": " + why));
        }

        for (std::size_t p = 0; p < N; ++p) {
            if (p == r)
                continue;
            auto& s = links_[p];
            d::set_no_sigpipe(s.fd());
            d::set_no_sigpipe(ctrl_[p].fd());
            s.set_nodelay(true);
            ctrl_[p].set_nodelay(true);
            ctrl_open_[p] = 1;
            if (socket_buffer_bytes_ > 0)
                s.set_buffer_sizes(socket_buffer_bytes_, socket_buffer_bytes_);
        }
        // Joiners held by a rank that is no longer the rendezvous look elsewhere
        if (rank_ != 0)
            for (auto& q : held_)
                if (q.kind == d::join_kind && q.s.valid())
                    reject_slot(q.s, q.slot, d::not_coordinator);
        held_.erase(std::remove_if(held_.begin(), held_.end(), [](const held_request& q) { return !q.s.valid(); }), held_.end());
        return ok();
    }

    // A joiner's wait on a rendezvous: what arrives on the rendezvous connection, while requests to
    // this rank (other joiners, survivors of an old membership) are turned away
    int await_admission(tcp_stream& s, detail::collective::clock::time_point first, detail::collective::clock::time_point dl,
                        std::vector<std::uint32_t>& view, std::uint64_t& epoch, unsigned& reason)
    {
        namespace d = detail::collective;
        bool waited = false;
        for (;;) {
            if (!waited)
                dl = std::min(dl, first);
            if (left_until(dl).count() == 0)
                return 0;
            pollfd pf[2] = {{s.fd(), POLLIN, 0}, {listener_.valid() ? listener_.fd() : -1, POLLIN, 0}};
            const int rc = ::poll(pf, 2, int(std::min<long long>(left_until(dl).count(), 1000)));
            if (rc < 0 && errno != EINTR)
                return 0;
            if (rc > 0 && (pf[1].revents & POLLIN)) {
                auto in = listener_.accept(std::chrono::milliseconds(0));
                d::hello h;
                if (in.valid() && read_hello(in, d::clock::now() + std::chrono::milliseconds(200), h))
                    reject(in, h, h.magic_ok && h.run_id != run_id_ ? d::bad_run_id : d::not_coordinator);
            }
            if (rc > 0 && pf[0].revents != 0) {
                const int got = read_view(s, dl, view, epoch, reason);
                if (got != 2)
                    return got;
                waited = true;
            }
        }
    }

    static collective_result<collective> join_impl(const collective_config& given_cfg, tcp_listener* given)
    {
        namespace d = detail::collective;
        using d::clock;
        collective_config cfg = given_cfg;
        cfg.elastic = true;
        auto made = setup(cfg, given, true);
        if (made.is_err())
            return made;
        collective c = std::move(made).unwrap();
        const auto dl = clock::now() + cfg.join_timeout;
        const backoff_policy policy{std::chrono::milliseconds(100), std::chrono::milliseconds(2000)};
        std::string last = "no rank answered";
        for (std::uint32_t attempt = 0; left_until(dl).count() > 0; ++attempt) {
            for (std::uint32_t cand = 0; cand < c.endpoints_.size() && left_until(dl).count() > 0; ++cand) {
                if (cand == c.slot_)
                    continue;
                auto s = tcp_stream::connect(c.eps_[cand].host, c.eps_[cand].port, std::min(c.probe_timeout_, left_until(dl)));
                if (!s.valid())
                    continue;
                d::hello h;
                h.kind = d::join_kind;
                h.from = c.slot_;
                h.to = cand;
                h.run_id = c.run_id_;
                const auto b = d::encode(h);
                if (!s.write_all(reinterpret_cast<const char*>(b.data()), b.size(), std::max(left_until(dl), std::chrono::milliseconds(1))))
                    continue;
                // The first answer within gather_window (a member answers at its next poll), then the view
                std::vector<std::uint32_t> view;
                std::uint64_t epoch = 0;
                unsigned reason = 0;
                const int got = c.await_admission(s, clock::now() + c.gather_window_, dl, view, epoch, reason);
                if (got < 0) {
                    if (reason == d::bad_run_id || reason == d::bad_magic)
                        return err(collective_error(collective_errc::handshake_rejected, int(cand),
                                                    "slot " + std::to_string(cand) + " (" + c.endpoints_[cand] + "): " + d::reason_text(reason)));
                    last = "slot " + std::to_string(cand) + ": " + d::reason_text(reason);
                    continue;
                }
                if (got == 0)
                    continue;
                if (std::find(view.begin(), view.end(), c.slot_) == view.end())
                    continue;
                c.adopt(epoch, view);
                if (auto e = c.build_mesh(std::min(dl, clock::now() + c.connect_timeout_)); e.is_err())
                    return err(e.unwrap_err_unchecked());
                return ok(std::move(c));
            }
            std::this_thread::sleep_for(std::min(backoff_ceiling(policy, attempt), left_until(dl)));
        }
        return err(collective_error(collective_errc::timed_out, -1,
                                    "slot " + std::to_string(c.slot_) + " was not admitted within " +
                                        std::to_string(cfg.join_timeout.count()) + " ms (" + last + ")"));
    }
};

} // namespace net
} // namespace snicholls

#endif // !_WIN32
#endif /* ts_moveables_net_collective_hpp */
