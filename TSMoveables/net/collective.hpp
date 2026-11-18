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
//  and connect again.
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
    broken,               // an earlier call failed; connect again
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
// 36 bytes each way, little-endian except the byte-order probe, which is the
// sender's native 0x01020304 so a peer of the other byte order reads it wrong.
// `channel` says which of the pair's two connections this is: 0 carries the
// data, 1 only ever carries an abort notice.
inline constexpr std::uint32_t hello_magic = 0x48534d54;   // "TMSH"
inline constexpr std::uint16_t hello_version = 1;
inline constexpr std::size_t hello_size = 36;
enum : unsigned char { hello_kind = 0, accept_kind = 1, reject_kind = 2 };
enum : unsigned char { ok_reason = 0, bad_magic, bad_world, bad_rank, duplicate_rank, bad_run_id, bad_byte_order };

inline const char* reason_text(unsigned r)
{
    switch (r) {
    case bad_magic: return "not a collective of this version";
    case bad_world: return "world size differs";
    case bad_rank: return "rank out of place";
    case duplicate_rank: return "rank already connected";
    case bad_run_id: return "run id differs";
    case bad_byte_order: return "byte order differs";
    default: return "refused";
    }
}

struct hello {
    unsigned char kind = hello_kind;
    unsigned char reason = ok_reason;
    unsigned char channel = 0;
    std::uint32_t world = 0, from = 0, to = 0;
    std::uint64_t run_id = 0;
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
inline constexpr std::size_t abort_size = 8;
inline constexpr std::uint32_t nobody = 0xffffffff;

// ----------------------------------------------------------- the op header
//
// 32 bytes to every peer at the start of every collective, little-endian.
inline constexpr std::uint32_t op_magic = 0x4f4d5354;      // "TSMO"
inline constexpr std::size_t op_header_size = 32;
enum : unsigned char { op_allreduce_sum = 1, op_broadcast = 2, op_barrier = 3 };
enum : unsigned char { dtype_none = 0, dtype_f32 = 1 };

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
    std::uint64_t seq = 0, n = 0;
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
    return b;
}

inline std::string describe(const unsigned char* b)
{
    if (get32(b) != op_magic)
        return "bytes that are not a collective header";
    std::string s = op_name(b[4]);
    s += " #" + std::to_string(get64(b + 16)) + " of " + std::to_string(get64(b + 24)) + " elements";
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
    /// and hands the ports round). Ignored on the last rank, which only dials.
    static collective_result<collective> connect(const collective_config& cfg, tcp_listener listener)
    {
        return connect_impl(cfg, &listener);
    }

    int rank() const noexcept { return rank_; }
    int size() const noexcept { return size_; }

    /// The endpoint of a rank, as configured.
    const std::string& endpoint(int r) const { return endpoints_.at(std::size_t(r)); }

    /// True after a failure: every call then returns collective_errc::broken.
    bool broken() const noexcept { return broken_.has_value(); }

    /// Collectives completed so far (each one's sequence number is checked
    /// against every peer's).
    std::uint64_t sequence() const noexcept { return seq_; }

    /// In place: every rank ends with, per element, ((x_0 + x_1) + ...) + x_{N-1}
    /// in rank order, in float. Bit-identical on every rank.
    collective_result<void> allreduce_sum(float* data, std::size_t n)
    {
        if (auto e = precheck(data, n); e.is_err())
            return e;
        namespace d = detail::collective;
        const std::size_t N = std::size_t(size_), r = std::size_t(rank_);
        if (N == 1) {
            ++seq_;
            return ok();
        }
        auto lanes = start(d::op_allreduce_sum, d::dtype_f32, 0, n);

        const std::size_t seg = segment_bytes_;
        const std::size_t K = 2;                                // segments of ring per peer
        const std::size_t mine = chunk_len(n, N, r) * sizeof(float);
        const std::size_t nseg = (mine + seg - 1) / seg;
        std::size_t reduced = 0;                                 // segments reduced
        std::size_t reduced_bytes = 0;
        std::size_t ring_limit = std::min(mine, K * seg);
        rings_.resize(N);
        for (std::size_t p = 0; p < N; ++p)
            if (p != r)
                rings_[p].resize(K * seg / sizeof(float));
        acc_.resize(seg / sizeof(float));

        char* bytes = reinterpret_cast<char*>(data);
        for (std::size_t p = 0; p < N; ++p) {
            if (p == r)
                continue;
            auto& l = lanes[p];
            l.out.push_back({bytes + chunk_begin(n, N, p) * sizeof(float), chunk_len(n, N, p) * sizeof(float)});
            l.out.push_back({bytes + chunk_begin(n, N, r) * sizeof(float), mine, 0, &reduced_bytes});
            l.in.push_back({reinterpret_cast<char*>(rings_[p].data()), mine, 0, &ring_limit, K * seg});
            l.in.push_back({bytes + chunk_begin(n, N, p) * sizeof(float), chunk_len(n, N, p) * sizeof(float)});
        }

        float* own = data + chunk_begin(n, N, r);
        auto reduce = [&]() -> bool {
            while (reduced < nseg) {
                const std::size_t end = std::min(mine, (reduced + 1) * seg);
                for (std::size_t p = 0; p < N; ++p)
                    if (p != r && lanes[p].in[1].done < end)
                        return false;
                const std::size_t first = reduced * seg / sizeof(float);
                const std::size_t count = (end - reduced * seg) / sizeof(float);
                const std::size_t slot = (reduced % K) * seg / sizeof(float);
                auto src = [&](std::size_t k) -> const float* {
                    return k == r ? own + first : rings_[k].data() + slot;
                };
                // Rank order, one rank per pass: acc = x0 + x1, then acc += x2, ...
                const float* a = src(0);
                const float* b = src(1);
                float* acc = acc_.data();
                for (std::size_t i = 0; i < count; ++i)
                    acc[i] = a[i] + b[i];
                for (std::size_t k = 2; k < N; ++k) {
                    const float* x = src(k);
                    for (std::size_t i = 0; i < count; ++i)
                        acc[i] += x[i];
                }
                std::memcpy(own + first, acc, count * sizeof(float));
                ++reduced;
                reduced_bytes = end;
                ring_limit = std::min(mine, (reduced + K) * seg);
            }
            return true;
        };
        return finish(run(lanes, reduce));
    }

    /// In place: every rank ends with root's n floats. The root sends the
    /// buffer once, in N-1 pieces; each other rank forwards its piece.
    collective_result<void> broadcast(float* data, std::size_t n, int root)
    {
        if (root < 0 || root >= size_)
            return err(collective_error(collective_errc::bad_config, -1,
                                        "broadcast root " + std::to_string(root) + " is not a rank"));
        if (auto e = precheck(data, n); e.is_err())
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
        if (auto e = precheck(nullptr, 0); e.is_err())
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

    int rank_ = 0;
    int size_ = 1;
    std::vector<std::string> endpoints_;
    std::vector<tcp_stream> links_;                  // links_[p]: the data connection to rank p (none to itself)
    std::vector<tcp_stream> ctrl_;                   // ctrl_[p]: the control connection, for abort notices
    std::vector<std::array<unsigned char, detail::collective::abort_size>> notice_;   // a notice arriving
    std::vector<std::size_t> notice_got_;
    std::vector<char> ctrl_open_;                    // still worth reading
    bool adopted_ = false;                           // the failure in hand came from another rank's notice
    std::chrono::milliseconds io_timeout_{30'000};
    std::size_t segment_bytes_ = std::size_t{1} << 20;
    std::uint64_t seq_ = 0;
    std::optional<collective_error> broken_;
    std::array<unsigned char, detail::collective::op_header_size> header_out_{};
    std::vector<std::array<unsigned char, detail::collective::op_header_size>> header_in_;
    std::vector<std::vector<float>> rings_;
    std::vector<float> acc_;

    std::string who(int p) const
    {
        return "rank " + std::to_string(p) + " (" + endpoints_[std::size_t(p)] + ")";
    }

    collective_result<void> precheck(const float* data, std::size_t n)
    {
        if (broken_)
            return err(collective_error(collective_errc::broken, broken_->peer(), broken_->what()));
        if (size_ < 1 || std::size_t(size_) != links_.size())
            return err(collective_error(collective_errc::bad_config, -1, "not connected"));
        if (n != 0 && data == nullptr)
            return err(collective_error(collective_errc::bad_config, -1, "null buffer"));
        if (n > std::numeric_limits<std::size_t>::max() / sizeof(float))
            return err(collective_error(collective_errc::bad_config, -1, "buffer too large"));
        return ok();
    }

    // Every lane starts by sending this rank's header and receiving the peer's
    lanes_t start(unsigned op, unsigned dtype, std::uint32_t root, std::size_t n)
    {
        namespace d = detail::collective;
        header_out_ = d::encode(d::op_header{op, dtype, root, std::uint32_t(size_), seq_, std::uint64_t(n)});
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
        const auto& b = notice_[i];
        const unsigned code = b[2];
        const std::uint32_t culprit = d::get32(b.data() + 4);
        if ((unsigned(b[0]) | unsigned(b[1]) << 8) != d::abort_magic || code < 1 ||
            code > unsigned(collective_errc::broken)) {
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

    static collective_result<collective> connect_impl(const collective_config& cfg, tcp_listener* given)
    {
        namespace d = detail::collective;
        using d::clock;
        const auto dl = clock::now() + cfg.connect_timeout;
        auto bad = [](const std::string& s) { return err(collective_error(collective_errc::bad_config, -1, s)); };

        const std::size_t N = cfg.peers.size();
        if (N == 0 || N > 0xffff)
            return bad("peers must list between 1 and 65535 ranks");
        if (cfg.rank < 0 || std::size_t(cfg.rank) >= N)
            return bad("rank " + std::to_string(cfg.rank) + " is not in 0.." + std::to_string(N - 1));
        if (cfg.io_timeout.count() <= 0 || cfg.connect_timeout.count() <= 0 || cfg.handshake_timeout.count() <= 0)
            return bad("timeouts must be positive");
        if (cfg.segment_bytes < sizeof(float) || cfg.segment_bytes > (std::size_t{1} << 30))
            return bad("segment_bytes must be between 4 bytes and 1 GiB");

        std::vector<d::endpoint> eps;
        for (const auto& s : cfg.peers) {
            auto e = d::parse_endpoint(s);
            if (!e)
                return bad("\"" + s + "\" is not host:port");
            eps.push_back(*e);
        }

        collective c;
        c.rank_ = cfg.rank;
        c.size_ = int(N);
        c.endpoints_ = cfg.peers;
        c.io_timeout_ = cfg.io_timeout;
        c.segment_bytes_ = cfg.segment_bytes / sizeof(float) * sizeof(float);
        c.links_.resize(N);
        c.ctrl_.resize(N);
        c.notice_.assign(N, {});
        c.notice_got_.assign(N, 0);
        c.ctrl_open_.assign(N, 0);
        if (N == 1)
            return ok(std::move(c));

        const std::size_t r = std::size_t(cfg.rank);
        tcp_listener listener;
        if (r + 1 < N) {
            if (given && given->valid())
                listener = std::move(*given);
            else {
                std::string e;
                listener = tcp_listener::bind(cfg.listen_host, eps[r].port, 128, &e);
                if (!listener.valid())
                    return err(collective_error(collective_errc::bind_failed, int(r),
                                                c.who(int(r)) + ": " + e));
            }
        }

        auto mine = [&](std::size_t to, unsigned char channel, unsigned char kind, unsigned char reason) {
            d::hello h;
            h.kind = kind;
            h.reason = reason;
            h.channel = channel;
            h.world = std::uint32_t(N);
            h.from = std::uint32_t(r);
            h.to = std::uint32_t(to);
            h.run_id = cfg.run_id;
            return d::encode(h);
        };

        // Every pair has two connections: channel 0 for data, channel 1 for abort notices
        auto link = [&c](std::size_t p, unsigned ch) -> tcp_stream& { return ch == 0 ? c.links_[p] : c.ctrl_[p]; };

        // 1. Dial every lower rank twice and say hello. Each of them listens before it dials anyone,
        //    so this waits only for processes to start, never on a cycle.
        const backoff_policy policy{std::chrono::milliseconds(20), std::chrono::milliseconds(500)};
        for (std::size_t p = 0; p < 2 * r; ++p) {
            const unsigned ch = unsigned(p % 2);
            const std::size_t to = p / 2;
            std::string last;
            for (std::uint32_t attempt = 0;; ++attempt) {
                const auto left = left_until(dl);
                if (left.count() == 0)
                    return err(collective_error(collective_errc::connect_failed, int(to),
                                                c.who(int(to)) + ": not reachable within " +
                                                    std::to_string(cfg.connect_timeout.count()) + " ms" +
                                                    (last.empty() ? "" : " (" + last + ")")));
                auto s = tcp_stream::connect(eps[to].host, eps[to].port,
                                             std::min(left, std::chrono::milliseconds(2000)), &last);
                if (s.valid()) {
                    link(to, ch) = std::move(s);
                    break;
                }
                std::this_thread::sleep_for(std::min(backoff_ceiling(policy, attempt), left_until(dl)));
            }
            const auto h = mine(to, static_cast<unsigned char>(ch), d::hello_kind, d::ok_reason);
            std::string e;
            if (!link(to, ch).write_all(reinterpret_cast<const char*>(h.data()), h.size(),
                                        std::max(left_until(dl), std::chrono::milliseconds(1)), &e))
                return err(collective_error(collective_errc::connect_failed, int(to), c.who(int(to)) + ": " + e));
        }

        // 2. Accept every higher rank: check its hello, answer it. A connection that is not one of
        //    this job's ranks is refused and closed, and the wait goes on for the real one.
        std::string refused;
        int refused_rank = -1;
        for (std::size_t accepted = 0; accepted < 2 * (N - 1 - r);) {
            std::string e;
            auto s = listener.accept(left_until(dl), &e);
            if (!s.valid()) {
                if (errno != ETIMEDOUT && left_until(dl).count() > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));   // EMFILE and friends
                    continue;
                }
                std::size_t missing = r + 1;
                while (missing < N && c.links_[missing].valid() && c.ctrl_[missing].valid())
                    ++missing;
                if (!refused.empty())
                    return err(collective_error(collective_errc::handshake_rejected, refused_rank,
                                                "waiting for " + c.who(int(missing)) + "; refused " + refused));
                return err(collective_error(collective_errc::timed_out, int(missing),
                                            c.who(int(missing)) + ": did not connect within " +
                                                std::to_string(cfg.connect_timeout.count()) + " ms"));
            }
            std::array<unsigned char, d::hello_size> buf{};
            const auto hdl = std::min(dl, clock::now() + cfg.handshake_timeout);
            if (read_exact(s, buf.data(), buf.size(), hdl, &e) != 1)
                continue;                                         // silent or gone: not one of ours
            const auto h = d::decode(buf.data());
            unsigned char why = d::ok_reason;
            if (!h.magic_ok || h.kind != d::hello_kind || h.channel > 1)
                why = d::bad_magic;
            else if (!h.native_order)
                why = d::bad_byte_order;
            else if (h.world != N)
                why = d::bad_world;
            else if (h.to != r || h.from <= r || h.from >= N)
                why = d::bad_rank;
            else if (h.run_id != cfg.run_id)
                why = d::bad_run_id;
            else if (link(h.from, h.channel).valid())
                why = d::duplicate_rank;
            const auto reply = mine(h.magic_ok ? h.from : 0, h.channel,
                                    why == d::ok_reason ? d::accept_kind : d::reject_kind, why);
            const bool sent = s.write_all(reinterpret_cast<const char*>(reply.data()), reply.size(),
                                          std::max(left_until(hdl), std::chrono::milliseconds(1)), &e);
            if (why != d::ok_reason) {
                refused_rank = h.magic_ok && h.from < N ? int(h.from) : -1;
                refused = "a connection claiming rank " + std::to_string(h.from) + ": " + d::reason_text(why);
                continue;
            }
            if (!sent)
                continue;
            link(h.from, h.channel) = std::move(s);
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
                                            c.who(int(p)) + ": no handshake answer" +
                                                (got == 0 ? " (connection closed)" : e.empty() ? "" : " (" + e + ")")));
            const auto h = d::decode(buf.data());
            const char* why = nullptr;
            if (!h.magic_ok)
                why = d::reason_text(d::bad_magic);
            else if (h.kind == d::reject_kind)
                why = d::reason_text(h.reason);
            else if (h.kind != d::accept_kind || h.from != p || h.to != r || h.channel != ch)
                why = d::reason_text(d::bad_rank);
            else if (h.world != N)
                why = d::reason_text(d::bad_world);
            else if (h.run_id != cfg.run_id)
                why = d::reason_text(d::bad_run_id);
            else if (!h.native_order)
                why = d::reason_text(d::bad_byte_order);
            if (why)
                return err(collective_error(collective_errc::handshake_rejected, int(p),
                                            c.who(int(p)) + ": " + why));
        }

        for (std::size_t p = 0; p < N; ++p) {
            if (p == r)
                continue;
            auto& s = c.links_[p];
            d::set_no_sigpipe(s.fd());
            d::set_no_sigpipe(c.ctrl_[p].fd());
            s.set_nodelay(true);
            c.ctrl_[p].set_nodelay(true);
            c.ctrl_open_[p] = 1;
            if (cfg.socket_buffer_bytes > 0)
                s.set_buffer_sizes(cfg.socket_buffer_bytes, cfg.socket_buffer_bytes);
        }
        return ok(std::move(c));
    }
};

} // namespace net
} // namespace snicholls

#endif // !_WIN32
#endif /* ts_moveables_net_collective_hpp */
