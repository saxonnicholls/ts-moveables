//
//  http2.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - HTTP/2 (RFC 9113) as a protocol delegate
//
//  The second protocol on the same two axes, and the one the interfaces were
//  shaped for. `deliver`, `respond`, `begin_stream` and `stream_write` have
//  carried a stream id since phase 1 - always 0 for HTTP/1.x - precisely so
//  that multiplexing needed no interface change. It did not: h2 streams map
//  straight onto the existing async `responder` and `response_stream`, which
//  already complete out of order because a moveable complete-once handle can
//  be answered from any thread at any time. That is the whole payoff of the
//  design, cashed in.
//
//      snicholls::http::server srv;
//      snicholls::http::enable_http2(srv);      // h2 by ALPN, h2c by preface
//      srv.get("/hello", [](const auto&, auto res) {
//          res.send(200, "text/plain", "hello");
//      });
//
//  Selection is per connection and at run time. `enable_http2` installs a
//  protocol factory that hands each new connection an `alpn_protocol`: a
//  three-line delegate that looks at what ALPN settled on, sniffs the client
//  preface when nothing did, and swaps itself out for `http2_protocol` or
//  `http1_protocol` through the same `switch_protocol` path WebSocket uses.
//  One binary serves both, chosen per connection, with no rebuild - which is
//  the thing a compile-time architecture structurally cannot do.
//
//  Two things carry most of the risk, so both are stated plainly:
//
//  HPACK (RFC 7541) is the bulk of the work and the least forgiving part of
//  it. The static table, the integer and string primitives, the dynamic table
//  with its eviction rules and the canonical Huffman code are all here, and
//  the Huffman tables are built by a `constexpr` function into `.rodata`
//  exactly as the HTTP/1.1 parser's character tables are - a decode step is
//  one indexed load, not a walk. The published RFC 7541 Appendix C vectors are
//  in the test suite because a decoder that only ever agrees with its own
//  encoder is a decoder that has not been checked.
//
//  Abuse limits are here from the first commit rather than after the first
//  incident. A decoded-header-list cap (the HPACK bomb), a bound on concurrent
//  streams, a cap on compressed CONTINUATION bytes (CVE-2024-27316) and token
//  buckets on RST_STREAM and on SETTINGS/PING (the 2023 Rapid Reset class) are
//  not extras: a multiplexed server without them is a DoS amplifier, because
//  the client can make it do unbounded work for a bounded number of bytes.
//
//  POSIX only, like everything downstream of the reactor; on Windows this
//  header compiles to nothing and SNICHOLLS_HAS_HTTP2 is 0.
//

#ifndef http2_hpp
#define http2_hpp

#include "server.hpp"

#if !SNICHOLLS_HAS_HTTP_SERVER
#define SNICHOLLS_HAS_HTTP2 0
#else
#define SNICHOLLS_HAS_HTTP2 1

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace snicholls {
namespace http {

// ============================================================== HPACK (7541)
//
// Header compression, in its own namespace because it is a self-contained
// codec with published test vectors and deserves to be testable on its own.

namespace hpack {

// ------------------------------------------------------------ Huffman tables
//
// RFC 7541 Appendix B: a canonical prefix code over 257 symbols (256 octets
// plus EOS), 5 to 30 bits per symbol.
//
// Both directions are compile-time tables, for the same reason the HTTP/1.1
// parser builds its token and hex tables with `make_char_tables()`: this is
// the hottest per-byte code in the header path, and a table that lands in
// .rodata costs one indexed load instead of a branch chain, with nothing to
// mispredict and no lazy initialisation to guard. `constexpr`, not
// `consteval` - this library is C++17, where `consteval` does not exist. It
// would say "compile time or fail" more exactly and generate identical code,
// so it is not worth a version fence.

struct huff_code {
    std::uint32_t code;
    std::uint8_t bits;
};

inline constexpr huff_code kHuffCodes[257] = {
    {0x00001ff8u, 13}, {0x007fffd8u, 23}, {0x0fffffe2u, 28}, {0x0fffffe3u, 28},
    {0x0fffffe4u, 28}, {0x0fffffe5u, 28}, {0x0fffffe6u, 28}, {0x0fffffe7u, 28},
    {0x0fffffe8u, 28}, {0x00ffffeau, 24}, {0x3ffffffcu, 30}, {0x0fffffe9u, 28},
    {0x0fffffeau, 28}, {0x3ffffffdu, 30}, {0x0fffffebu, 28}, {0x0fffffecu, 28},
    {0x0fffffedu, 28}, {0x0fffffeeu, 28}, {0x0fffffefu, 28}, {0x0ffffff0u, 28},
    {0x0ffffff1u, 28}, {0x0ffffff2u, 28}, {0x3ffffffeu, 30}, {0x0ffffff3u, 28},
    {0x0ffffff4u, 28}, {0x0ffffff5u, 28}, {0x0ffffff6u, 28}, {0x0ffffff7u, 28},
    {0x0ffffff8u, 28}, {0x0ffffff9u, 28}, {0x0ffffffau, 28}, {0x0ffffffbu, 28},
    {0x00000014u,  6}, {0x000003f8u, 10}, {0x000003f9u, 10}, {0x00000ffau, 12},
    {0x00001ff9u, 13}, {0x00000015u,  6}, {0x000000f8u,  8}, {0x000007fau, 11},
    {0x000003fau, 10}, {0x000003fbu, 10}, {0x000000f9u,  8}, {0x000007fbu, 11},
    {0x000000fau,  8}, {0x00000016u,  6}, {0x00000017u,  6}, {0x00000018u,  6},
    {0x00000000u,  5}, {0x00000001u,  5}, {0x00000002u,  5}, {0x00000019u,  6},
    {0x0000001au,  6}, {0x0000001bu,  6}, {0x0000001cu,  6}, {0x0000001du,  6},
    {0x0000001eu,  6}, {0x0000001fu,  6}, {0x0000005cu,  7}, {0x000000fbu,  8},
    {0x00007ffcu, 15}, {0x00000020u,  6}, {0x00000ffbu, 12}, {0x000003fcu, 10},
    {0x00001ffau, 13}, {0x00000021u,  6}, {0x0000005du,  7}, {0x0000005eu,  7},
    {0x0000005fu,  7}, {0x00000060u,  7}, {0x00000061u,  7}, {0x00000062u,  7},
    {0x00000063u,  7}, {0x00000064u,  7}, {0x00000065u,  7}, {0x00000066u,  7},
    {0x00000067u,  7}, {0x00000068u,  7}, {0x00000069u,  7}, {0x0000006au,  7},
    {0x0000006bu,  7}, {0x0000006cu,  7}, {0x0000006du,  7}, {0x0000006eu,  7},
    {0x0000006fu,  7}, {0x00000070u,  7}, {0x00000071u,  7}, {0x00000072u,  7},
    {0x000000fcu,  8}, {0x00000073u,  7}, {0x000000fdu,  8}, {0x00001ffbu, 13},
    {0x0007fff0u, 19}, {0x00001ffcu, 13}, {0x00003ffcu, 14}, {0x00000022u,  6},
    {0x00007ffdu, 15}, {0x00000003u,  5}, {0x00000023u,  6}, {0x00000004u,  5},
    {0x00000024u,  6}, {0x00000005u,  5}, {0x00000025u,  6}, {0x00000026u,  6},
    {0x00000027u,  6}, {0x00000006u,  5}, {0x00000074u,  7}, {0x00000075u,  7},
    {0x00000028u,  6}, {0x00000029u,  6}, {0x0000002au,  6}, {0x00000007u,  5},
    {0x0000002bu,  6}, {0x00000076u,  7}, {0x0000002cu,  6}, {0x00000008u,  5},
    {0x00000009u,  5}, {0x0000002du,  6}, {0x00000077u,  7}, {0x00000078u,  7},
    {0x00000079u,  7}, {0x0000007au,  7}, {0x0000007bu,  7}, {0x00007ffeu, 15},
    {0x000007fcu, 11}, {0x00003ffdu, 14}, {0x00001ffdu, 13}, {0x0ffffffcu, 28},
    {0x000fffe6u, 20}, {0x003fffd2u, 22}, {0x000fffe7u, 20}, {0x000fffe8u, 20},
    {0x003fffd3u, 22}, {0x003fffd4u, 22}, {0x003fffd5u, 22}, {0x007fffd9u, 23},
    {0x003fffd6u, 22}, {0x007fffdau, 23}, {0x007fffdbu, 23}, {0x007fffdcu, 23},
    {0x007fffddu, 23}, {0x007fffdeu, 23}, {0x00ffffebu, 24}, {0x007fffdfu, 23},
    {0x00ffffecu, 24}, {0x00ffffedu, 24}, {0x003fffd7u, 22}, {0x007fffe0u, 23},
    {0x00ffffeeu, 24}, {0x007fffe1u, 23}, {0x007fffe2u, 23}, {0x007fffe3u, 23},
    {0x007fffe4u, 23}, {0x001fffdcu, 21}, {0x003fffd8u, 22}, {0x007fffe5u, 23},
    {0x003fffd9u, 22}, {0x007fffe6u, 23}, {0x007fffe7u, 23}, {0x00ffffefu, 24},
    {0x003fffdau, 22}, {0x001fffddu, 21}, {0x000fffe9u, 20}, {0x003fffdbu, 22},
    {0x003fffdcu, 22}, {0x007fffe8u, 23}, {0x007fffe9u, 23}, {0x001fffdeu, 21},
    {0x007fffeau, 23}, {0x003fffddu, 22}, {0x003fffdeu, 22}, {0x00fffff0u, 24},
    {0x001fffdfu, 21}, {0x003fffdfu, 22}, {0x007fffebu, 23}, {0x007fffecu, 23},
    {0x001fffe0u, 21}, {0x001fffe1u, 21}, {0x003fffe0u, 22}, {0x001fffe2u, 21},
    {0x007fffedu, 23}, {0x003fffe1u, 22}, {0x007fffeeu, 23}, {0x007fffefu, 23},
    {0x000fffeau, 20}, {0x003fffe2u, 22}, {0x003fffe3u, 22}, {0x003fffe4u, 22},
    {0x007ffff0u, 23}, {0x003fffe5u, 22}, {0x003fffe6u, 22}, {0x007ffff1u, 23},
    {0x03ffffe0u, 26}, {0x03ffffe1u, 26}, {0x000fffebu, 20}, {0x0007fff1u, 19},
    {0x003fffe7u, 22}, {0x007ffff2u, 23}, {0x003fffe8u, 22}, {0x01ffffecu, 25},
    {0x03ffffe2u, 26}, {0x03ffffe3u, 26}, {0x03ffffe4u, 26}, {0x07ffffdeu, 27},
    {0x07ffffdfu, 27}, {0x03ffffe5u, 26}, {0x00fffff1u, 24}, {0x01ffffedu, 25},
    {0x0007fff2u, 19}, {0x001fffe3u, 21}, {0x03ffffe6u, 26}, {0x07ffffe0u, 27},
    {0x07ffffe1u, 27}, {0x03ffffe7u, 26}, {0x07ffffe2u, 27}, {0x00fffff2u, 24},
    {0x001fffe4u, 21}, {0x001fffe5u, 21}, {0x03ffffe8u, 26}, {0x03ffffe9u, 26},
    {0x0ffffffdu, 28}, {0x07ffffe3u, 27}, {0x07ffffe4u, 27}, {0x07ffffe5u, 27},
    {0x000fffecu, 20}, {0x00fffff3u, 24}, {0x000fffedu, 20}, {0x001fffe6u, 21},
    {0x003fffe9u, 22}, {0x001fffe7u, 21}, {0x001fffe8u, 21}, {0x007ffff3u, 23},
    {0x003fffeau, 22}, {0x003fffebu, 22}, {0x01ffffeeu, 25}, {0x01ffffefu, 25},
    {0x00fffff4u, 24}, {0x00fffff5u, 24}, {0x03ffffeau, 26}, {0x007ffff4u, 23},
    {0x03ffffebu, 26}, {0x07ffffe6u, 27}, {0x03ffffecu, 26}, {0x03ffffedu, 26},
    {0x07ffffe7u, 27}, {0x07ffffe8u, 27}, {0x07ffffe9u, 27}, {0x07ffffeau, 27},
    {0x07ffffebu, 27}, {0x0ffffffeu, 28}, {0x07ffffecu, 27}, {0x07ffffedu, 27},
    {0x07ffffeeu, 27}, {0x07ffffefu, 27}, {0x07fffff0u, 27}, {0x03ffffeeu, 26},
    {0x3fffffffu, 30},
};

// One decode step: consume four bits, land in a new state, and maybe emit a
// byte. Four bits rather than one because a whole nibble can complete at most
// one symbol - the shortest code is five bits, so after an emission at most
// three bits remain and no second symbol can fit - which keeps the step a
// fixed-size record and the decoder a flat loop.
//
//   flags bit 0  a symbol was completed and is in `sym`
//   flags bit 1  the input is invalid here (an encoded EOS symbol)
//   flags bit 2  `next` is a legal place for the string to end
struct huff_step {
    std::uint8_t next;
    std::uint8_t sym;
    std::uint8_t flags;
};

enum : std::uint8_t { huff_emit = 1, huff_fail = 2, huff_end_ok = 4 };

// 256 states of 16 nibbles: 12 KiB of .rodata, built once by the compiler.
struct huff_dfa {
    huff_step step[256][16];
};

// A prefix code over 257 symbols has exactly 256 internal nodes, so a state
// fits in a byte and the whole machine addresses in 8 bits.
constexpr huff_dfa make_huff_dfa() noexcept
{
    // child[node][bit]: 0 absent, >0 an internal node, <0 the leaf -(sym+1).
    // Value-initialised, which in a constexpr function is a zero fill the
    // evaluator does once rather than a loop it has to step through.
    int child[256][2]{};
    int used = 0;
    for (int s = 0; s < 257; ++s) {
        const std::uint32_t code = kHuffCodes[s].code;
        const int len = int(kHuffCodes[s].bits);
        int node = 0;
        for (int i = len - 1; i >= 0; --i) {
            const int bit = int((code >> i) & 1u);
            if (i == 0) {
                child[node][bit] = -(s + 1);
            } else {
                if (child[node][bit] == 0)
                    child[node][bit] = ++used;
                node = child[node][bit];
            }
        }
    }

    // RFC 7541 §5.2: the only legal trailing bits are the most significant
    // bits of EOS - which is all ones - and there must be fewer than eight of
    // them. Those are exactly the nodes on the all-ones path at depth 1..7.
    bool ends_ok[256]{};
    ends_ok[0] = true;                          // a whole number of symbols
    int n = 0;
    for (int d = 0; d < 7; ++d) {
        const int c = child[n][1];
        if (c <= 0)
            break;
        n = c;
        ends_ok[n] = true;
    }

    huff_dfa t{};
    for (int st = 0; st < 256; ++st) {
        for (int nib = 0; nib < 16; ++nib) {
            int node = st;
            bool fail = false, emit = false;
            int sym = 0;
            for (int b = 3; b >= 0; --b) {
                const int c = child[node][(nib >> b) & 1];
                if (c == 0) {                   // unreachable: the code is complete
                    fail = true;
                    break;
                }
                if (c < 0) {
                    const int leaf = -c - 1;
                    if (leaf == 256) {          // an encoded EOS is a decoding error
                        fail = true;
                        break;
                    }
                    sym = leaf;
                    emit = true;
                    node = 0;
                } else {
                    node = c;
                }
            }
            huff_step& e = t.step[st][nib];
            e.next = std::uint8_t(fail ? 0 : node);
            e.sym = std::uint8_t(sym);
            e.flags = std::uint8_t((emit ? huff_emit : 0) | (fail ? huff_fail : 0) |
                                   ((!fail && ends_ok[node]) ? huff_end_ok : 0));
        }
    }
    return t;
}

inline constexpr huff_dfa kHuffDfa = make_huff_dfa();

// The compiler-enforced proof that the machine above is a constant rather
// than start-up work: a static_assert cannot read a table that does not exist
// until main() runs. The properties chosen are not arbitrary - they are §5.2's
// padding rule and one end-to-end decode, so this is a correctness check that
// happens to also prove the timing.
static_assert((kHuffDfa.step[0][0x0].flags & huff_end_ok) == 0,
              "four trailing zero bits are not legal Huffman padding");
static_assert((kHuffDfa.step[0][0xf].flags & huff_end_ok) != 0,
              "four trailing one bits are legal Huffman padding");
static_assert((kHuffDfa.step[kHuffDfa.step[0][0x0].next][0x7].flags & huff_emit) != 0,
              "the five-bit code 00000 must complete a symbol");
static_assert(kHuffDfa.step[kHuffDfa.step[0][0x0].next][0x7].sym == '0',
              "00000 is the code for '0' (RFC 7541 Appendix B)");

// Appends to `out`. False on an encoded EOS, on illegal padding, or when the
// result would exceed `max_out` - the last of which is the decompression-bomb
// bound, enforced while decoding rather than after it.
inline bool huff_decode(const unsigned char* p, std::size_t n, std::string& out,
                        std::size_t max_out)
{
    std::uint8_t st = 0;
    bool end_ok = true;
    for (std::size_t i = 0; i < n; ++i) {
        const huff_step& hi = kHuffDfa.step[st][p[i] >> 4];
        if (hi.flags & huff_fail)
            return false;
        if (hi.flags & huff_emit) {
            if (out.size() >= max_out)
                return false;
            out.push_back(char(hi.sym));
        }
        st = hi.next;

        const huff_step& lo = kHuffDfa.step[st][p[i] & 0x0f];
        if (lo.flags & huff_fail)
            return false;
        if (lo.flags & huff_emit) {
            if (out.size() >= max_out)
                return false;
            out.push_back(char(lo.sym));
        }
        st = lo.next;
        end_ok = (lo.flags & huff_end_ok) != 0;
    }
    return end_ok;
}

inline std::size_t huff_encoded_size(const char* p, std::size_t n) noexcept
{
    std::size_t bits = 0;
    for (std::size_t i = 0; i < n; ++i)
        bits += kHuffCodes[static_cast<unsigned char>(p[i])].bits;
    return (bits + 7) / 8;
}

inline void huff_encode(const char* p, std::size_t n, std::string& out)
{
    std::uint64_t acc = 0;
    int held = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const huff_code& c = kHuffCodes[static_cast<unsigned char>(p[i])];
        acc = (acc << c.bits) | c.code;
        held += c.bits;
        while (held >= 8) {
            held -= 8;
            out.push_back(char((acc >> held) & 0xff));
        }
    }
    if (held) {                                 // pad with the MSBs of EOS: ones
        acc = (acc << (8 - held)) | ((1u << (8 - held)) - 1u);
        out.push_back(char(acc & 0xff));
    }
}

// -------------------------------------------------------------- static table
//
// RFC 7541 Appendix A. Pointers to string literals, so the whole table is a
// compile-time constant in .rodata with nothing to construct at startup.

struct static_entry {
    const char* name;
    const char* value;
};

inline constexpr static_entry kStatic[61] = {
    {":authority", ""},
    {":method", "GET"},
    {":method", "POST"},
    {":path", "/"},
    {":path", "/index.html"},
    {":scheme", "http"},
    {":scheme", "https"},
    {":status", "200"},
    {":status", "204"},
    {":status", "206"},
    {":status", "304"},
    {":status", "400"},
    {":status", "404"},
    {":status", "500"},
    {"accept-charset", ""},
    {"accept-encoding", "gzip, deflate"},
    {"accept-language", ""},
    {"accept-ranges", ""},
    {"accept", ""},
    {"access-control-allow-origin", ""},
    {"age", ""},
    {"allow", ""},
    {"authorization", ""},
    {"cache-control", ""},
    {"content-disposition", ""},
    {"content-encoding", ""},
    {"content-language", ""},
    {"content-length", ""},
    {"content-location", ""},
    {"content-range", ""},
    {"content-type", ""},
    {"cookie", ""},
    {"date", ""},
    {"etag", ""},
    {"expect", ""},
    {"expires", ""},
    {"from", ""},
    {"host", ""},
    {"if-match", ""},
    {"if-modified-since", ""},
    {"if-none-match", ""},
    {"if-range", ""},
    {"if-unmodified-since", ""},
    {"last-modified", ""},
    {"link", ""},
    {"location", ""},
    {"max-forwards", ""},
    {"proxy-authenticate", ""},
    {"proxy-authorization", ""},
    {"range", ""},
    {"referer", ""},
    {"refresh", ""},
    {"retry-after", ""},
    {"server", ""},
    {"set-cookie", ""},
    {"strict-transport-security", ""},
    {"transfer-encoding", ""},
    {"user-agent", ""},
    {"vary", ""},
    {"via", ""},
    {"www-authenticate", ""},
};

inline constexpr std::size_t kStaticCount = 61;

// A compile-time strlen, so the lengths sit beside the pointers in .rodata
// rather than being recomputed on every comparison
constexpr std::size_t clen(const char* s) noexcept
{
    std::size_t n = 0;
    while (s[n])
        ++n;
    return n;
}

struct static_lengths {
    std::uint8_t name[kStaticCount]{};
    std::uint8_t value[kStaticCount]{};
};

constexpr static_lengths make_static_lengths() noexcept
{
    static_lengths t{};
    for (std::size_t i = 0; i < kStaticCount; ++i) {
        t.name[i] = std::uint8_t(clen(kStatic[i].name));
        t.value[i] = std::uint8_t(clen(kStatic[i].value));
    }
    return t;
}

inline constexpr static_lengths kStaticLen = make_static_lengths();

static_assert(kStaticLen.name[0] == 10 && kStaticLen.value[1] == 3,
              "the static table's lengths are computed at compile time");

// ------------------------------------------------------------- dynamic table

// RFC 7541 §4: entries cost their two strings plus 32 bytes of accounting
// overhead, newest first, evicted from the back when the budget is exceeded.
class dynamic_table {
public:
    using entry = std::pair<std::string, std::string>;

    static std::size_t cost(const std::string& n, const std::string& v) noexcept
    {
        return n.size() + v.size() + 32;
    }

    std::size_t max_size() const noexcept { return max_; }
    std::size_t size() const noexcept { return size_; }
    std::size_t count() const noexcept { return v_.size(); }

    void max_size(std::size_t m)
    {
        max_ = m;
        evict();
    }

    // 0-based, newest first
    const entry* at(std::size_t i) const noexcept
    {
        return i < v_.size() ? &v_[i] : nullptr;
    }

    void insert(std::string n, std::string v)
    {
        const std::size_t c = cost(n, v);
        // §4.4: an entry larger than the whole table empties it and is not
        // added. That is not an error - it is how an encoder says "forget
        // everything", and treating it as one is a real interop bug.
        if (c > max_) {
            v_.clear();
            size_ = 0;
            return;
        }
        v_.emplace_front(std::move(n), std::move(v));
        size_ += c;
        evict();
    }

    void clear() noexcept
    {
        v_.clear();
        size_ = 0;
    }

private:
    void evict()
    {
        while (size_ > max_ && !v_.empty()) {
            size_ -= cost(v_.back().first, v_.back().second);
            v_.pop_back();
        }
    }

    std::deque<entry> v_;
    std::size_t size_ = 0;
    std::size_t max_ = 4096;
};

// ------------------------------------------------------------------ integers

// RFC 7541 §5.1. The continuation run is bounded twice over: by the value
// ceiling and by the shift, because an unbounded run of 0x80 octets is a
// denial of service that costs the attacker nothing.
inline bool read_int(const unsigned char*& p, const unsigned char* end,
                     unsigned prefix_bits, std::uint64_t& out) noexcept
{
    if (p >= end)
        return false;
    const unsigned mask = (1u << prefix_bits) - 1u;
    std::uint64_t v = std::uint64_t(*p++ & mask);
    if (v < mask) {
        out = v;
        return true;
    }
    unsigned shift = 0;
    for (;;) {
        if (p >= end || shift > 28)
            return false;
        const unsigned b = *p++;
        v += std::uint64_t(b & 0x7fu) << shift;
        if (v > 0x7fffffffull)                  // no index, length or table size is this big
            return false;
        if (!(b & 0x80u))
            break;
        shift += 7;
    }
    out = v;
    return true;
}

inline void write_int(std::string& out, unsigned prefix_bits, unsigned char high,
                      std::uint64_t v)
{
    const unsigned mask = (1u << prefix_bits) - 1u;
    if (v < mask) {
        out.push_back(char(high | unsigned(v)));
        return;
    }
    out.push_back(char(high | mask));
    v -= mask;
    while (v >= 128) {
        out.push_back(char((v & 0x7f) | 0x80));
        v >>= 7;
    }
    out.push_back(char(v));
}

inline bool read_string(const unsigned char*& p, const unsigned char* end,
                        std::string& out, std::size_t max_len)
{
    if (p >= end)
        return false;
    const bool huffman = (*p & 0x80u) != 0;
    std::uint64_t len = 0;
    if (!read_int(p, end, 7, len))
        return false;
    if (len > std::uint64_t(end - p))
        return false;
    out.clear();
    if (huffman) {
        if (!huff_decode(p, std::size_t(len), out, max_len))
            return false;
    } else {
        if (len > max_len)
            return false;
        out.assign(reinterpret_cast<const char*>(p), std::size_t(len));
    }
    p += std::size_t(len);
    return true;
}

// Huffman only when it actually shortens the field, which is what the RFC's
// own examples do and what keeps a pathological value from growing
inline void write_string(std::string& out, const std::string& s)
{
    const std::size_t h = huff_encoded_size(s.data(), s.size());
    if (h < s.size()) {
        write_int(out, 7, 0x80, h);
        huff_encode(s.data(), s.size(), out);
    } else {
        write_int(out, 7, 0x00, s.size());
        out += s;
    }
}

// ------------------------------------------------------------------- decoder

using field = std::pair<std::string, std::string>;

enum class status {
    ok,
    compression_error,      // connection error: the HPACK state is unrecoverable
    list_too_large          // stream error: state is intact, the field list is not
};

class decoder {
public:
    // The table starts at whatever we first advertised, before either side
    // has said anything
    void initial_table_size(std::size_t n)
    {
        settings_max_ = n;
        table_.max_size(n);
    }

    // What we told the peer in SETTINGS_HEADER_TABLE_SIZE: a dynamic table
    // size update above this is a compression error, not a negotiation. A
    // later reduction clamps the table but does not raise it - only the
    // peer's encoder may do that, and only through a size update.
    void settings_table_size(std::size_t n)
    {
        settings_max_ = n;
        if (table_.max_size() > n)
            table_.max_size(n);
    }

    const dynamic_table& table() const noexcept { return table_; }
    dynamic_table& table() noexcept { return table_; }

    // Decodes one complete header block. `max_list_size` bounds the *decoded*
    // size (RFC 7541's name+value+32 metric) - the HPACK-bomb cap. Going over
    // it stops the output growing but never stops the decode: the dynamic
    // table is connection state shared with the peer's encoder, so a block
    // that is refused must still be applied or every later block is garbage.
    status decode(const unsigned char* p, const unsigned char* end,
                  std::vector<field>& out, std::size_t max_list_size,
                  std::size_t max_fields)
    {
        bool allow_size_update = true;      // §4.2: only at the start of a block
        bool over = false;
        std::size_t list_size = 0;
        std::string name, value;

        // The bound on any single string is the *block* it came out of, not
        // the header-list cap. Those are different jobs: the list cap decides
        // whether we will serve the request, and refusing it is a stream
        // error; a string longer than its own block can only be a decoding
        // error. Conflating them turns "your headers are too big" into a
        // killed connection. A Huffman code of at least five bits per octet
        // cannot expand by more than 8/5, so twice the block is never binding
        // on legitimate input - the real memory bound is the caller's cap on
        // compressed header-block bytes.
        const std::size_t max_str = std::size_t(end - p) * 2 + 64;

        while (p < end) {
            const unsigned char b = *p;

            if (b & 0x80u) {                                    // §6.1 indexed
                allow_size_update = false;
                std::uint64_t idx = 0;
                if (!read_int(p, end, 7, idx))
                    return status::compression_error;
                if (idx == 0 || !lookup(std::size_t(idx), name, value))
                    return status::compression_error;
                add(out, name, value, list_size, max_list_size, max_fields, over);
                continue;
            }

            if ((b & 0xc0u) == 0x40u) {                         // §6.2.1 with indexing
                allow_size_update = false;
                std::uint64_t idx = 0;
                if (!read_int(p, end, 6, idx))
                    return status::compression_error;
                if (idx) {
                    if (!lookup_name(std::size_t(idx), name))
                        return status::compression_error;
                } else if (!read_string(p, end, name, max_str)) {
                    return status::compression_error;
                }
                if (!read_string(p, end, value, max_str))
                    return status::compression_error;
                table_.insert(name, value);
                add(out, name, value, list_size, max_list_size, max_fields, over);
                continue;
            }

            if ((b & 0xe0u) == 0x20u) {                         // §6.3 table size update
                if (!allow_size_update)
                    return status::compression_error;
                std::uint64_t sz = 0;
                if (!read_int(p, end, 5, sz))
                    return status::compression_error;
                if (sz > settings_max_)
                    return status::compression_error;
                table_.max_size(std::size_t(sz));
                continue;
            }

            // §6.2.2 without indexing and §6.2.3 never indexed. A server has
            // no reason to distinguish them on the way in: neither touches the
            // dynamic table, and "never indexed" is an instruction to
            // intermediaries about re-encoding, which we are not doing.
            allow_size_update = false;
            std::uint64_t idx = 0;
            if (!read_int(p, end, 4, idx))
                return status::compression_error;
            if (idx) {
                if (!lookup_name(std::size_t(idx), name))
                    return status::compression_error;
            } else if (!read_string(p, end, name, max_str)) {
                return status::compression_error;
            }
            if (!read_string(p, end, value, max_str))
                return status::compression_error;
            add(out, name, value, list_size, max_list_size, max_fields, over);
        }
        return over ? status::list_too_large : status::ok;
    }

private:
    static void add(std::vector<field>& out, const std::string& n, const std::string& v,
                    std::size_t& list_size, std::size_t max_list_size,
                    std::size_t max_fields, bool& over)
    {
        const std::size_t c = n.size() + v.size() + 32;
        if (over || out.size() >= max_fields || list_size + c > max_list_size) {
            over = true;
            return;
        }
        list_size += c;
        out.emplace_back(n, v);
    }

    bool lookup(std::size_t idx, std::string& n, std::string& v) const
    {
        if (idx <= kStaticCount) {
            n.assign(kStatic[idx - 1].name, kStaticLen.name[idx - 1]);
            v.assign(kStatic[idx - 1].value, kStaticLen.value[idx - 1]);
            return true;
        }
        const dynamic_table::entry* e = table_.at(idx - kStaticCount - 1);
        if (!e)
            return false;
        n = e->first;
        v = e->second;
        return true;
    }

    bool lookup_name(std::size_t idx, std::string& n) const
    {
        if (idx <= kStaticCount) {
            n.assign(kStatic[idx - 1].name, kStaticLen.name[idx - 1]);
            return true;
        }
        const dynamic_table::entry* e = table_.at(idx - kStaticCount - 1);
        if (!e)
            return false;
        n = e->first;
        return true;
    }

    dynamic_table table_;
    std::size_t settings_max_ = 4096;
};

// ------------------------------------------------------------------- encoder
//
// Static-table indexing plus literals without indexing. Deliberately no
// dynamic table on the way out: a server's response headers repeat little
// across streams that the static table does not already cover, and an encoder
// that never inserts cannot desynchronise a decoder - which removes an entire
// class of bug for a few bytes per response. The peer's table size still has
// to be honoured, because RFC 7541 §4.2 requires the change to be signalled.
class encoder {
public:
    void peer_table_size(std::size_t n)
    {
        if (n != peer_max_) {
            peer_max_ = n;
            signal_pending_ = true;
        }
    }

    void encode(const std::vector<field>& fields, std::string& out)
    {
        if (signal_pending_) {
            // We hold nothing, so the honest signal is a table of zero
            write_int(out, 5, 0x20, 0);
            signal_pending_ = false;
        }
        for (const auto& f : fields) {
            std::size_t name_idx = 0;
            std::size_t pair_idx = 0;
            find_static(f.first, f.second, name_idx, pair_idx);
            if (pair_idx) {
                write_int(out, 7, 0x80, pair_idx);
                continue;
            }
            if (name_idx) {
                write_int(out, 4, 0x00, name_idx);
            } else {
                out.push_back('\0');            // literal name, no indexing
                write_string(out, f.first);
            }
            write_string(out, f.second);
        }
    }

private:
    static void find_static(const std::string& n, const std::string& v,
                            std::size_t& name_idx, std::size_t& pair_idx) noexcept
    {
        name_idx = 0;
        pair_idx = 0;
        for (std::size_t i = 0; i < kStaticCount; ++i) {
            if (n.size() != kStaticLen.name[i] ||
                std::memcmp(n.data(), kStatic[i].name, n.size()) != 0)
                continue;
            if (!name_idx)
                name_idx = i + 1;
            if (v.size() == kStaticLen.value[i] &&
                (v.empty() || std::memcmp(v.data(), kStatic[i].value, v.size()) == 0)) {
                pair_idx = i + 1;
                return;
            }
        }
    }

    std::size_t peer_max_ = 4096;
    bool signal_pending_ = false;
};

} // namespace hpack

// ================================================================ HTTP/2 wire

// RFC 9113 §3.4. Twenty-four octets that cannot be the start of any valid
// HTTP/1.x request, which is exactly why they are there.
inline constexpr char kH2Preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
inline constexpr std::size_t kH2PrefaceLen = 24;

// How much of it is enough to commit. "PRI * HTTP/2.0\r\n" is already not a
// request any HTTP/1.x server could serve - the version alone is a 505 - so
// once those sixteen octets match there is nothing to be gained by hedging,
// and a great deal to be gained by handing the connection to the delegate
// that can answer a broken preface with GOAWAY instead of an HTTP/1.1 error
// page the client will not parse.
inline constexpr std::size_t kH2PrefaceCommit = 16;

enum class h2_frame : std::uint8_t {
    data = 0x0, headers = 0x1, priority = 0x2, rst_stream = 0x3, settings = 0x4,
    push_promise = 0x5, ping = 0x6, goaway = 0x7, window_update = 0x8,
    continuation = 0x9
};

enum : std::uint8_t {
    h2_flag_end_stream = 0x01,
    h2_flag_ack = 0x01,             // SETTINGS and PING reuse bit 0
    h2_flag_end_headers = 0x04,
    h2_flag_padded = 0x08,
    h2_flag_priority = 0x20
};

enum class h2_error : std::uint32_t {
    no_error = 0x0, protocol_error = 0x1, internal_error = 0x2,
    flow_control_error = 0x3, settings_timeout = 0x4, stream_closed = 0x5,
    frame_size_error = 0x6, refused_stream = 0x7, cancel = 0x8,
    compression_error = 0x9, connect_error = 0xa, enhance_your_calm = 0xb,
    inadequate_security = 0xc, http_1_1_required = 0xd
};

enum : std::uint16_t {
    h2_settings_header_table_size = 0x1,
    h2_settings_enable_push = 0x2,
    h2_settings_max_concurrent_streams = 0x3,
    h2_settings_initial_window_size = 0x4,
    h2_settings_max_frame_size = 0x5,
    h2_settings_max_header_list_size = 0x6
};

// Everything a connection is allowed to cost us. The defaults are deliberately
// conservative: an HTTP/2 server's whole risk profile is that one connection
// can ask for unbounded work, and every field here is a place someone has
// already found a way to do that.
struct h2_config {
    std::uint32_t header_table_size = 4096;         // SETTINGS_HEADER_TABLE_SIZE
    std::uint32_t max_concurrent_streams = 100;     // SETTINGS_MAX_CONCURRENT_STREAMS
    std::uint32_t initial_window_size = 65535;      // SETTINGS_INITIAL_WINDOW_SIZE
    std::uint32_t max_frame_size = 16384;           // SETTINGS_MAX_FRAME_SIZE
    std::uint32_t max_header_list_size = 32u * 1024;// SETTINGS_MAX_HEADER_LIST_SIZE
    std::uint32_t connection_window = 1024u * 1024; // our receive window, raised by WINDOW_UPDATE

    std::size_t max_header_fields = 128;            // decoded fields per request
    std::size_t max_header_block = 64u * 1024;      // *compressed* bytes across CONTINUATIONs

    // Rapid Reset (2023) and its relatives: a client that can make the server
    // open and tear down streams, or answer control frames, faster than it can
    // send bytes has an amplifier. Both are token buckets - a burst is normal
    // traffic, a sustained rate is not.
    double rst_burst = 200.0;
    double rst_per_second = 20.0;
    double control_burst = 200.0;                   // SETTINGS and PING together
    double control_per_second = 50.0;
};

namespace detail {

// A leaky bucket over steady_clock. Cheap enough to consult per frame: one
// clock read and two multiplies, on a path that is already doing IO.
class h2_bucket {
public:
    void configure(double burst, double per_second) noexcept
    {
        burst_ = burst;
        rate_ = per_second;
        tokens_ = burst;
    }

    bool take(std::chrono::steady_clock::time_point now) noexcept
    {
        if (last_.time_since_epoch().count() == 0)
            last_ = now;
        const double dt = std::chrono::duration<double>(now - last_).count();
        last_ = now;
        tokens_ = std::min(burst_, tokens_ + dt * rate_);
        if (tokens_ < 1.0)
            return false;
        tokens_ -= 1.0;
        return true;
    }

private:
    double burst_ = 200.0;
    double rate_ = 20.0;
    double tokens_ = 200.0;
    std::chrono::steady_clock::time_point last_{};
};

// RFC 9113 §5.1. `idle` never appears in the map - a stream that has not been
// opened has no state to store - and `reserved` never appears at all, because
// we do not push.
enum class h2_stream_state { open, half_closed_remote, half_closed_local, closed };

struct h2_stream {
    std::uint32_t id = 0;
    h2_stream_state state = h2_stream_state::open;

    std::int64_t send_window = 65535;       // what the peer will let us send
    std::int64_t recv_window = 65535;       // what we will let the peer send

    request req;
    bool have_headers = false;              // the request head has been decoded
    bool head_request = false;
    bool delivered = false;
    bool awaiting = false;                  // handed to a handler, no answer yet
    bool streaming = false;                 // an open response_stream

    bool has_content_length = false;
    std::uint64_t content_length = 0;
    std::uint64_t body_bytes = 0;

    std::string out_buf;                    // response body the send window will not take yet
    std::size_t out_off = 0;
    bool out_end = false;                   // END_STREAM once out_buf drains
    bool end_sent = false;
    bool headers_sent = false;

    std::chrono::steady_clock::time_point opened{};
};

} // namespace detail

// ---------------------------------------------------------------- the protocol

class http2_protocol final : public protocol_delegate {
public:
    explicit http2_protocol(const parse_limits& lim, h2_config cfg = h2_config{},
                            bool preface_already_seen = false)
        : lim_(lim), cfg_(cfg), preface_seen_(preface_already_seen)
    {
        dec_.initial_table_size(cfg_.header_table_size);
        conn_recv_window_ = std::int64_t(std::max<std::uint32_t>(cfg_.connection_window, 65535));
        rst_bucket_.configure(cfg_.rst_burst, cfg_.rst_per_second);
        ctl_bucket_.configure(cfg_.control_burst, cfg_.control_per_second);
    }

    const char* name() const noexcept override { return "h2"; }
    bool close_after_flush() const noexcept override { return close_; }
    std::size_t last_response_bytes() const noexcept override { return last_bytes_; }

    // Any stream still owed an answer. The sweeper uses this to keep a
    // connection whose handler went off to a thread pool from being reaped.
    bool busy() const noexcept override
    {
        for (const auto& kv : streams_)
            if (kv.second->awaiting)
                return true;
        return false;
    }

    // Deliberately false, always. `receiving()` drives the request-timeout
    // sweep, and that sweep answers with a literal HTTP/1.1 408 - which on an
    // h2 connection is not a timeout, it is a framing error the client cannot
    // parse. Half-finished requests are timed out from inside consume()
    // instead, per stream and with RST_STREAM, which is both correct for the
    // protocol and better behaviour: one slow stream does not kill the other
    // ninety-nine multiplexed onto the same socket.
    bool receiving() const noexcept override { return false; }

    bool consume(std::string& in, connection_host& host) override
    {
        if (close_) {
            in.clear();
            return false;
        }
        if (!preface_seen_) {
            const std::size_t have = std::min(in.size(), kH2PrefaceLen);
            if (std::memcmp(in.data(), kH2Preface, have) != 0) {
                connection_error(host, h2_error::protocol_error, "bad connection preface");
                in.clear();
                return false;
            }
            if (in.size() < kH2PrefaceLen)
                return true;
            in.erase(0, kH2PrefaceLen);
            preface_seen_ = true;
        }
        if (!settings_sent_) {
            send_settings(host);
            settings_sent_ = true;
        }

        std::size_t off = 0;
        while (!close_) {
            if (in.size() - off < 9)
                break;
            const unsigned char* p = reinterpret_cast<const unsigned char*>(in.data()) + off;
            const std::uint32_t len = (std::uint32_t(p[0]) << 16) |
                                      (std::uint32_t(p[1]) << 8) | std::uint32_t(p[2]);
            const std::uint8_t type = p[3];
            const std::uint8_t flags = p[4];
            // The reserved high bit of the stream id is ignored on receipt,
            // not validated: RFC 9113 §4.1 is explicit about it
            const std::uint32_t sid = ((std::uint32_t(p[5]) & 0x7fu) << 24) |
                                      (std::uint32_t(p[6]) << 16) |
                                      (std::uint32_t(p[7]) << 8) | std::uint32_t(p[8]);

            if (len > cfg_.max_frame_size) {
                connection_error(host, h2_error::frame_size_error, "frame exceeds SETTINGS_MAX_FRAME_SIZE");
                break;
            }
            if (in.size() - off < 9u + len)
                break;                          // the whole frame, or nothing

            if (!first_frame_seen_) {
                first_frame_seen_ = true;
                if (type != std::uint8_t(h2_frame::settings) || (flags & h2_flag_ack)) {
                    connection_error(host, h2_error::protocol_error,
                                     "the preface must be followed by SETTINGS");
                    break;
                }
            }
            // §6.10: a header block is atomic on the wire. Nothing at all may
            // come between HEADERS and its CONTINUATIONs - not another
            // stream's frames, not even PING - because the HPACK state would
            // be applied out of order.
            if (cont_stream_ &&
                (type != std::uint8_t(h2_frame::continuation) || sid != cont_stream_)) {
                connection_error(host, h2_error::protocol_error,
                                 "frame interleaved with a header block");
                break;
            }

            handle_frame(type, flags, sid, p + 9, len, host);
            off += 9u + len;
        }
        if (off)
            in.erase(0, off);

        expire_streams(host);
        flush_window_updates(host);
        pump(host);
        return !close_;
    }

    // ---- answering
    //
    // Every one of these is keyed by the stream id the interface has carried
    // since phase 1. Nothing here needed a new entry point, and nothing here
    // assumes the answers arrive in the order the requests did.

    void respond(std::uint64_t sid, response&& res, connection_host& host) override
    {
        last_bytes_ = 0;
        detail::h2_stream* s = find(std::uint32_t(sid));
        if (!s || s->end_sent)
            return;
        s->awaiting = false;
        const bool no_body = s->head_request || res.body.empty();
        // h2 frames carry the length, so Content-Length is not required here
        // the way it is on HTTP/1.1 - but clients and caches still read it,
        // and a HEAD answer has no other way to report the size it elided
        if (!res.headers.has("content-length") && res.status != 204 && res.status != 304 &&
            !(res.status >= 100 && res.status < 200)) {
            std::string n;
            detail::append_uint(n, res.body.size());
            res.headers.set("content-length", n);
        }
        send_response_headers(*s, res, no_body, host);
        if (!no_body) {
            s->out_buf = std::move(res.body);
            s->out_off = 0;
            s->out_end = true;
        } else {
            s->end_sent = true;
            close_local(*s);
        }
        last_bytes_ += pump(host);
    }

    bool begin_stream(std::uint64_t sid, response&& head, connection_host& host) override
    {
        last_bytes_ = 0;
        detail::h2_stream* s = find(std::uint32_t(sid));
        if (!s || s->end_sent)
            return false;
        s->awaiting = false;
        s->streaming = true;
        // No Content-Length and no chunked framing: h2 lengths are the frames
        send_response_headers(*s, head, /*end_stream*/ false, host);
        return true;
    }

    void stream_write(std::uint64_t sid, const char* data, std::size_t n,
                      connection_host& host) override
    {
        detail::h2_stream* s = find(std::uint32_t(sid));
        if (!s || !s->streaming || s->end_sent || n == 0 || s->head_request)
            return;
        compact(*s);
        s->out_buf.append(data, n);
        last_bytes_ += pump(host);
    }

    void end_stream(std::uint64_t sid, connection_host& host) override
    {
        detail::h2_stream* s = find(std::uint32_t(sid));
        if (!s || !s->streaming || s->end_sent)
            return;
        s->streaming = false;
        s->out_end = true;
        last_bytes_ += pump(host);
    }

    // ---- observation, for tests and metrics
    std::size_t open_streams() const noexcept { return streams_.size(); }
    std::int64_t connection_send_window() const noexcept { return conn_send_window_; }
    bool goaway_sent() const noexcept { return goaway_sent_; }
    h2_error last_error() const noexcept { return last_error_; }

private:
    // ------------------------------------------------------------- emitting

    void put(std::string& o, std::uint32_t len, h2_frame type, std::uint8_t flags,
             std::uint32_t sid) const
    {
        o.push_back(char((len >> 16) & 0xff));
        o.push_back(char((len >> 8) & 0xff));
        o.push_back(char(len & 0xff));
        o.push_back(char(std::uint8_t(type)));
        o.push_back(char(flags));
        o.push_back(char((sid >> 24) & 0x7f));
        o.push_back(char((sid >> 16) & 0xff));
        o.push_back(char((sid >> 8) & 0xff));
        o.push_back(char(sid & 0xff));
    }

    static void put32(std::string& o, std::uint32_t v)
    {
        o.push_back(char((v >> 24) & 0xff));
        o.push_back(char((v >> 16) & 0xff));
        o.push_back(char((v >> 8) & 0xff));
        o.push_back(char(v & 0xff));
    }

    static std::uint32_t get32(const unsigned char* p) noexcept
    {
        return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
               (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
    }

    void emit(connection_host& host, const std::string& frame)
    {
        host.write_app(frame.data(), frame.size());
        last_bytes_ += frame.size();
    }

    void send_settings(connection_host& host)
    {
        std::string o;
        const std::uint16_t ids[6] = {
            h2_settings_header_table_size, h2_settings_enable_push,
            h2_settings_max_concurrent_streams, h2_settings_initial_window_size,
            h2_settings_max_frame_size, h2_settings_max_header_list_size};
        const std::uint32_t vals[6] = {
            cfg_.header_table_size, 0u, cfg_.max_concurrent_streams,
            cfg_.initial_window_size, cfg_.max_frame_size, cfg_.max_header_list_size};
        put(o, 36, h2_frame::settings, 0, 0);
        for (int i = 0; i < 6; ++i) {
            o.push_back(char((ids[i] >> 8) & 0xff));
            o.push_back(char(ids[i] & 0xff));
            put32(o, vals[i]);
        }
        // The connection receive window starts at 65535 whatever SETTINGS
        // says; raising it takes a WINDOW_UPDATE on stream 0
        const std::int64_t want = std::int64_t(std::max<std::uint32_t>(cfg_.connection_window, 65535));
        if (want > 65535)
            put_window_update(o, 0, std::uint32_t(want - 65535));
        emit(host, o);
    }

    void put_window_update(std::string& o, std::uint32_t sid, std::uint32_t inc) const
    {
        put(o, 4, h2_frame::window_update, 0, sid);
        put32(o, inc);
    }

    void send_rst(connection_host& host, std::uint32_t sid, h2_error code)
    {
        std::string o;
        put(o, 4, h2_frame::rst_stream, 0, sid);
        put32(o, std::uint32_t(code));
        emit(host, o);
    }

    void connection_error(connection_host& host, h2_error code, const char* why)
    {
        if (goaway_sent_)
            return;
        goaway_sent_ = true;
        close_ = true;
        last_error_ = code;
        const std::size_t why_len = why ? std::strlen(why) : 0;
        std::string o;
        put(o, std::uint32_t(8 + why_len), h2_frame::goaway, 0, 0);
        put32(o, last_peer_stream_);
        put32(o, std::uint32_t(code));
        if (why_len)
            o.append(why, why_len);
        emit(host, o);
    }

    // A stream error: the connection survives, this exchange does not
    void stream_error(connection_host& host, std::uint32_t sid, h2_error code)
    {
        send_rst(host, sid, code);
        forget(sid);
    }

    // ------------------------------------------------------------ streams

    detail::h2_stream* find(std::uint32_t sid) noexcept
    {
        auto it = streams_.find(sid);
        return it == streams_.end() ? nullptr : it->second.get();
    }

    // Streams are held by shared_ptr for one specific reason: `deliver` hands
    // the handler a `const request&` that lives inside the stream, and a
    // handler that answers inline gets all the way back round to closing that
    // stream while it is still holding the reference. A strong local reference
    // across the call means the object outlives the erase, and the handler
    // reading the request after answering is merely odd rather than undefined.
    std::shared_ptr<detail::h2_stream> hold(std::uint32_t sid) const
    {
        auto it = streams_.find(sid);
        return it == streams_.end() ? std::shared_ptr<detail::h2_stream>() : it->second;
    }

    void forget(std::uint32_t sid)
    {
        streams_.erase(sid);
        remember_closed(sid);
    }

    // A bounded memory of streams we have finished with, so that a frame
    // arriving after the close is answered STREAM_CLOSED rather than being
    // mistaken for a brand-new stream (§5.1) or for one that never existed
    void remember_closed(std::uint32_t sid)
    {
        if (std::find(closed_.begin(), closed_.end(), sid) != closed_.end())
            return;
        closed_.push_back(sid);
        if (closed_.size() > 256)
            closed_.pop_front();
    }

    bool was_closed(std::uint32_t sid) const noexcept
    {
        return std::find(closed_.begin(), closed_.end(), sid) != closed_.end();
    }

    void close_local(detail::h2_stream& s)
    {
        if (s.state == detail::h2_stream_state::half_closed_remote) {
            forget(s.id);
            return;
        }
        s.state = detail::h2_stream_state::half_closed_local;
    }

    static void compact(detail::h2_stream& s)
    {
        if (s.out_off && s.out_off * 2 >= s.out_buf.size()) {
            s.out_buf.erase(0, s.out_off);
            s.out_off = 0;
        }
    }

    // Half-finished requests are a per-stream problem, so they get a
    // per-stream answer. Checked here rather than from the connection sweeper
    // because the sweeper's remedy is an HTTP/1.1 error line.
    void expire_streams(connection_host& host)
    {
        const auto limit = host.config().request_timeout;
        if (close_ || limit.count() <= 0 || streams_.empty())
            return;
        const auto now = std::chrono::steady_clock::now();
        std::vector<std::uint32_t> doomed;
        for (auto& kv : streams_) {
            detail::h2_stream& s = *kv.second;
            if (s.delivered || s.awaiting)
                continue;                       // a handler owns it now
            if (now - s.opened > limit)
                doomed.push_back(s.id);
        }
        for (std::uint32_t sid : doomed)
            stream_error(host, sid, h2_error::enhance_your_calm);
    }

    // ------------------------------------------------------ inbound windows

    // Replenish eagerly. We buffer whole request bodies up to the parser's
    // max_body, so holding the window closed would stall the peer for no
    // benefit; the real bound on memory is max_body times max_concurrent
    // streams, which is a configuration decision rather than a window one.
    void consumed(std::uint32_t sid, std::uint32_t n)
    {
        conn_recv_window_ -= std::int64_t(n);
        conn_pending_update_ += n;
        auto it = streams_.find(sid);
        if (it != streams_.end()) {
            it->second->recv_window -= std::int64_t(n);
            stream_updates_[sid] += n;
        }
    }

    void flush_window_updates(connection_host& host)
    {
        if (close_)
            return;
        std::string o;
        if (conn_pending_update_) {
            put_window_update(o, 0, conn_pending_update_);
            conn_recv_window_ += std::int64_t(conn_pending_update_);
            conn_pending_update_ = 0;
        }
        for (auto& kv : stream_updates_) {
            auto it = streams_.find(kv.first);
            if (it == streams_.end() || kv.second == 0)
                continue;
            put_window_update(o, kv.first, kv.second);
            it->second->recv_window += std::int64_t(kv.second);
        }
        stream_updates_.clear();
        if (!o.empty())
            host.write_app(o.data(), o.size());
    }

    // ----------------------------------------------------- outbound windows

    // Sends whatever the windows currently allow, across every stream that has
    // something queued, and returns how many bytes went out. The count matters
    // only to the response path: the access log attributes bytes to the
    // exchange that produced them, and a pump driven by an incoming
    // WINDOW_UPDATE belongs to no exchange in particular.
    std::size_t pump(connection_host& host)
    {
        if (close_)
            return 0;
        std::string o;
        for (auto it = streams_.begin(); it != streams_.end();) {
            detail::h2_stream& s = *it->second;
            bool finished = false;
            while (!s.end_sent) {
                const std::size_t left = s.out_buf.size() - s.out_off;
                if (left == 0) {
                    if (!s.out_end)
                        break;                  // a streaming producer has more to come
                    put(o, 0, h2_frame::data, h2_flag_end_stream, s.id);
                    s.end_sent = true;
                    break;
                }
                const std::int64_t window = std::min(conn_send_window_, s.send_window);
                if (window <= 0)
                    break;                      // blocked; a WINDOW_UPDATE will restart us
                std::size_t n = std::min<std::size_t>(left, std::size_t(window));
                n = std::min<std::size_t>(n, peer_max_frame_);
                const bool last = (n == left) && s.out_end;
                put(o, std::uint32_t(n), h2_frame::data, last ? h2_flag_end_stream : 0, s.id);
                o.append(s.out_buf, s.out_off, n);
                s.out_off += n;
                conn_send_window_ -= std::int64_t(n);
                s.send_window -= std::int64_t(n);
                if (last)
                    s.end_sent = true;
            }
            compact(s);                         // before any erase: `s` may not survive it
            if (s.end_sent && !s.awaiting && !s.streaming) {
                if (s.state == detail::h2_stream_state::half_closed_remote) {
                    remember_closed(s.id);
                    it = streams_.erase(it);
                    finished = true;
                } else {
                    s.state = detail::h2_stream_state::half_closed_local;
                }
            }
            if (!finished)
                ++it;
        }
        if (!o.empty())
            host.write_app(o.data(), o.size());
        return o.size();
    }

    // -------------------------------------------------------- frame handling

    void handle_frame(std::uint8_t type, std::uint8_t flags, std::uint32_t sid,
                      const unsigned char* p, std::uint32_t len, connection_host& host)
    {
        switch (type) {
        case std::uint8_t(h2_frame::data):          on_data(flags, sid, p, len, host); break;
        case std::uint8_t(h2_frame::headers):       on_headers(flags, sid, p, len, host); break;
        case std::uint8_t(h2_frame::priority):      on_priority(sid, p, len, host); break;
        case std::uint8_t(h2_frame::rst_stream):    on_rst(sid, p, len, host); break;
        case std::uint8_t(h2_frame::settings):      on_settings(flags, sid, p, len, host); break;
        case std::uint8_t(h2_frame::push_promise):
            // §8.4: a client must never send one, and a server that tolerates
            // it has handed the client a way to create server-side state
            connection_error(host, h2_error::protocol_error, "PUSH_PROMISE from a client");
            break;
        case std::uint8_t(h2_frame::ping):          on_ping(flags, sid, p, len, host); break;
        case std::uint8_t(h2_frame::goaway):        on_goaway(sid, len, host); break;
        case std::uint8_t(h2_frame::window_update): on_window_update(sid, p, len, host); break;
        case std::uint8_t(h2_frame::continuation):  on_continuation(flags, sid, p, len, host); break;
        default:
            // §5.5: an unknown frame type is discarded, which is what lets the
            // protocol be extended without breaking us
            break;
        }
    }

    void on_data(std::uint8_t flags, std::uint32_t sid, const unsigned char* p,
                 std::uint32_t len, connection_host& host)
    {
        if (sid == 0) {
            connection_error(host, h2_error::protocol_error, "DATA on stream 0");
            return;
        }
        // Flow control is accounted before anything else can reject the frame:
        // the peer has spent the window whether or not we like the stream
        consumed(sid, len);
        if (conn_recv_window_ < 0) {
            connection_error(host, h2_error::flow_control_error, "connection receive window exceeded");
            return;
        }

        std::uint32_t off = 0, pad = 0;
        if (flags & h2_flag_padded) {
            if (len < 1) {
                connection_error(host, h2_error::protocol_error, "padded DATA with no pad length");
                return;
            }
            pad = p[0];
            off = 1;
            if (std::uint32_t(pad) + 1u > len) {
                connection_error(host, h2_error::protocol_error, "DATA padding exceeds the frame");
                return;
            }
        }

        detail::h2_stream* s = find(sid);
        if (!s) {
            if (was_closed(sid) || sid <= last_peer_stream_)
                stream_error(host, sid, h2_error::stream_closed);
            else
                connection_error(host, h2_error::protocol_error, "DATA on an idle stream");
            return;
        }
        if (s->state != detail::h2_stream_state::open &&
            s->state != detail::h2_stream_state::half_closed_local) {
            stream_error(host, sid, h2_error::stream_closed);
            return;
        }
        if (s->recv_window < 0) {
            stream_error(host, sid, h2_error::flow_control_error);
            return;
        }

        const std::uint32_t n = len - off - pad;
        if (s->req.body.size() + n > lim_.max_body) {
            stream_error(host, sid, h2_error::enhance_your_calm);
            return;
        }
        s->req.body.append(reinterpret_cast<const char*>(p + off), n);
        s->body_bytes += n;

        if (flags & h2_flag_end_stream)
            end_of_request(sid, host);
    }

    void on_headers(std::uint8_t flags, std::uint32_t sid, const unsigned char* p,
                    std::uint32_t len, connection_host& host)
    {
        if (sid == 0) {
            connection_error(host, h2_error::protocol_error, "HEADERS on stream 0");
            return;
        }
        if ((sid & 1u) == 0) {
            connection_error(host, h2_error::protocol_error, "even stream identifier from a client");
            return;
        }

        std::uint32_t off = 0, pad = 0;
        if (flags & h2_flag_padded) {
            if (len < 1) {
                connection_error(host, h2_error::protocol_error, "padded HEADERS with no pad length");
                return;
            }
            pad = p[0];
            off = 1;
        }
        if (flags & h2_flag_priority) {
            if (len < off + 5u) {
                connection_error(host, h2_error::frame_size_error, "HEADERS priority block truncated");
                return;
            }
            const std::uint32_t dep = get32(p + off) & 0x7fffffffu;
            off += 5;
            if (dep == sid) {
                // §5.3.1 in RFC 7540; priority is deprecated by RFC 9113 but a
                // self-dependency is still nonsense and still worth refusing
                stream_error(host, sid, h2_error::protocol_error);
                return;
            }
        }
        if (off + pad > len) {
            connection_error(host, h2_error::protocol_error, "HEADERS padding exceeds the frame");
            return;
        }

        detail::h2_stream* s = find(sid);
        if (s) {
            if (s->state != detail::h2_stream_state::open) {
                stream_error(host, sid, h2_error::stream_closed);
                return;
            }
            if (!(flags & h2_flag_end_stream)) {
                // Trailers arrive as HEADERS with END_STREAM; anything else on
                // an open stream is a second request head, which is malformed
                stream_error(host, sid, h2_error::protocol_error);
                return;
            }
            trailers_ = true;                   // decoded for HPACK state, then discarded
        } else {
            if (was_closed(sid) || sid <= last_peer_stream_) {
                // §5.1.1: identifiers only ever go up, and a reused one is a
                // desynchronised peer rather than a new request
                connection_error(host, h2_error::protocol_error,
                                 "stream identifier reused or out of order");
                return;
            }
            if (goaway_received_) {
                stream_error(host, sid, h2_error::refused_stream);
                return;
            }
            if (streams_.size() >= cfg_.max_concurrent_streams) {
                send_rst(host, sid, h2_error::refused_stream);
                last_peer_stream_ = sid;
                remember_closed(sid);
                return;
            }
            trailers_ = false;
            auto ns = std::make_shared<detail::h2_stream>();
            ns->id = sid;
            ns->send_window = std::int64_t(peer_initial_window_);
            ns->recv_window = std::int64_t(cfg_.initial_window_size);
            ns->opened = std::chrono::steady_clock::now();
            s = ns.get();
            streams_.emplace(sid, std::move(ns));
        }
        last_peer_stream_ = std::max(last_peer_stream_, sid);

        if (len - off - pad > cfg_.max_header_block) {
            // Only reachable when the compressed cap is configured below
            // SETTINGS_MAX_FRAME_SIZE, but the two are independent knobs and
            // the check belongs wherever the block grows
            connection_error(host, h2_error::enhance_your_calm, "header block too large");
            return;
        }
        block_.assign(reinterpret_cast<const char*>(p + off), len - off - pad);
        block_stream_ = sid;
        block_end_stream_ = (flags & h2_flag_end_stream) != 0;
        if (flags & h2_flag_end_headers) {
            cont_stream_ = 0;
            finish_header_block(host);
        } else {
            cont_stream_ = sid;
        }
    }

    void on_continuation(std::uint8_t flags, std::uint32_t sid, const unsigned char* p,
                         std::uint32_t len, connection_host& host)
    {
        if (cont_stream_ == 0 || sid != cont_stream_) {
            connection_error(host, h2_error::protocol_error, "CONTINUATION without HEADERS");
            return;
        }
        // The CONTINUATION flood (CVE-2024-27316): an unbounded run of frames
        // that never sets END_HEADERS costs the attacker almost nothing and
        // costs us memory and HPACK work without ever creating a request the
        // rest of the server could account for.
        if (block_.size() + len > cfg_.max_header_block) {
            connection_error(host, h2_error::enhance_your_calm, "header block too large");
            return;
        }
        block_.append(reinterpret_cast<const char*>(p), len);
        if (flags & h2_flag_end_headers) {
            cont_stream_ = 0;
            finish_header_block(host);
        }
    }

    void on_priority(std::uint32_t sid, const unsigned char* p, std::uint32_t len,
                     connection_host& host)
    {
        if (sid == 0) {
            connection_error(host, h2_error::protocol_error, "PRIORITY on stream 0");
            return;
        }
        if (len != 5) {
            stream_error(host, sid, h2_error::frame_size_error);
            return;
        }
        if ((get32(p) & 0x7fffffffu) == sid) {
            stream_error(host, sid, h2_error::protocol_error);
            return;
        }
        // Parsed, validated and ignored. RFC 9113 deprecates the priority
        // scheme outright; honouring it would be implementing a mechanism the
        // standard has withdrawn.
    }

    void on_rst(std::uint32_t sid, const unsigned char* p, std::uint32_t len,
                connection_host& host)
    {
        (void)p;
        if (sid == 0) {
            connection_error(host, h2_error::protocol_error, "RST_STREAM on stream 0");
            return;
        }
        if (len != 4) {
            connection_error(host, h2_error::frame_size_error, "RST_STREAM length is not 4");
            return;
        }
        detail::h2_stream* s = find(sid);
        if (!s && !was_closed(sid) && sid > last_peer_stream_) {
            connection_error(host, h2_error::protocol_error, "RST_STREAM on an idle stream");
            return;
        }
        // Rapid Reset: open a stream, cancel it, repeat. Each cycle is nine
        // bytes to the client and a whole request to us, so the only defence
        // that works is a rate limit on the cancels themselves.
        if (!rst_bucket_.take(std::chrono::steady_clock::now())) {
            connection_error(host, h2_error::enhance_your_calm, "RST_STREAM flood");
            return;
        }
        if (s)
            forget(sid);
    }

    void on_settings(std::uint8_t flags, std::uint32_t sid, const unsigned char* p,
                     std::uint32_t len, connection_host& host)
    {
        if (sid != 0) {
            connection_error(host, h2_error::protocol_error, "SETTINGS on a non-zero stream");
            return;
        }
        if (flags & h2_flag_ack) {
            if (len != 0)
                connection_error(host, h2_error::frame_size_error, "SETTINGS ACK with a payload");
            return;
        }
        if (len % 6 != 0) {
            connection_error(host, h2_error::frame_size_error, "SETTINGS length is not a multiple of 6");
            return;
        }
        // Every SETTINGS obliges us to send an ACK, which is the amplification
        if (!ctl_bucket_.take(std::chrono::steady_clock::now())) {
            connection_error(host, h2_error::enhance_your_calm, "SETTINGS flood");
            return;
        }

        for (std::uint32_t i = 0; i + 6 <= len; i += 6) {
            const std::uint16_t id = std::uint16_t((std::uint32_t(p[i]) << 8) | p[i + 1]);
            const std::uint32_t v = get32(p + i + 2);
            switch (id) {
            case h2_settings_header_table_size:
                enc_.peer_table_size(v);
                break;
            case h2_settings_enable_push:
                if (v > 1) {
                    connection_error(host, h2_error::protocol_error, "SETTINGS_ENABLE_PUSH is not 0 or 1");
                    return;
                }
                break;
            case h2_settings_max_concurrent_streams:
                break;                          // a limit on what we may push; we never do
            case h2_settings_initial_window_size: {
                if (v > 0x7fffffffu) {
                    connection_error(host, h2_error::flow_control_error,
                                     "SETTINGS_INITIAL_WINDOW_SIZE above 2^31-1");
                    return;
                }
                // §6.9.2: the change is a *delta* applied to every open
                // stream, not a reset - a stream that has already sent data
                // keeps that spend
                const std::int64_t delta = std::int64_t(v) - std::int64_t(peer_initial_window_);
                peer_initial_window_ = v;
                for (auto& kv : streams_) {
                    kv.second->send_window += delta;
                    if (kv.second->send_window > 0x7fffffffLL) {
                        connection_error(host, h2_error::flow_control_error,
                                         "stream send window overflowed");
                        return;
                    }
                }
                break;
            }
            case h2_settings_max_frame_size:
                if (v < 16384u || v > 16777215u) {
                    connection_error(host, h2_error::protocol_error, "SETTINGS_MAX_FRAME_SIZE out of range");
                    return;
                }
                peer_max_frame_ = v;
                break;
            case h2_settings_max_header_list_size:
                peer_max_header_list_ = v;
                break;
            default:
                break;                          // §6.5.2: unknown settings are ignored
            }
        }

        std::string o;
        put(o, 0, h2_frame::settings, h2_flag_ack, 0);
        emit(host, o);
        pump(host);
    }

    void on_ping(std::uint8_t flags, std::uint32_t sid, const unsigned char* p,
                 std::uint32_t len, connection_host& host)
    {
        if (sid != 0) {
            connection_error(host, h2_error::protocol_error, "PING on a non-zero stream");
            return;
        }
        if (len != 8) {
            connection_error(host, h2_error::frame_size_error, "PING length is not 8");
            return;
        }
        if (flags & h2_flag_ack)
            return;
        if (!ctl_bucket_.take(std::chrono::steady_clock::now())) {
            connection_error(host, h2_error::enhance_your_calm, "PING flood");
            return;
        }
        std::string o;
        put(o, 8, h2_frame::ping, h2_flag_ack, 0);
        o.append(reinterpret_cast<const char*>(p), 8);
        emit(host, o);
    }

    void on_goaway(std::uint32_t sid, std::uint32_t len, connection_host& host)
    {
        if (sid != 0) {
            connection_error(host, h2_error::protocol_error, "GOAWAY on a non-zero stream");
            return;
        }
        if (len < 8) {
            connection_error(host, h2_error::frame_size_error, "GOAWAY shorter than 8 octets");
            return;
        }
        // Finish what is in flight, refuse anything new, then close
        goaway_received_ = true;
        if (streams_.empty())
            close_ = true;
    }

    void on_window_update(std::uint32_t sid, const unsigned char* p, std::uint32_t len,
                          connection_host& host)
    {
        if (len != 4) {
            connection_error(host, h2_error::frame_size_error, "WINDOW_UPDATE length is not 4");
            return;
        }
        const std::uint32_t inc = get32(p) & 0x7fffffffu;
        if (sid == 0) {
            if (inc == 0) {
                connection_error(host, h2_error::protocol_error, "WINDOW_UPDATE increment of 0");
                return;
            }
            conn_send_window_ += std::int64_t(inc);
            if (conn_send_window_ > 0x7fffffffLL) {
                connection_error(host, h2_error::flow_control_error, "connection send window overflowed");
                return;
            }
            pump(host);
            return;
        }
        detail::h2_stream* s = find(sid);
        if (!s) {
            if (was_closed(sid) || sid <= last_peer_stream_)
                return;                         // a window for a stream that is over: harmless
            connection_error(host, h2_error::protocol_error, "WINDOW_UPDATE on an idle stream");
            return;
        }
        if (inc == 0) {
            stream_error(host, sid, h2_error::protocol_error);
            return;
        }
        s->send_window += std::int64_t(inc);
        if (s->send_window > 0x7fffffffLL) {
            stream_error(host, sid, h2_error::flow_control_error);
            return;
        }
        pump(host);
    }

    // ------------------------------------------------------- header blocks

    void finish_header_block(connection_host& host)
    {
        const std::uint32_t sid = block_stream_;
        fields_.clear();
        const unsigned char* b = reinterpret_cast<const unsigned char*>(block_.data());
        const hpack::status st = dec_.decode(b, b + block_.size(), fields_,
                                             cfg_.max_header_list_size, cfg_.max_header_fields);
        block_.clear();
        block_stream_ = 0;

        if (st == hpack::status::compression_error) {
            // §4.3: HPACK state is shared with the peer's encoder, so a bad
            // block is not a bad request - it is a connection that can never
            // be interpreted again
            connection_error(host, h2_error::compression_error, "HPACK decoding failed");
            return;
        }

        detail::h2_stream* s = find(sid);
        if (!s)
            return;                             // reset while its header block was in flight

        if (st == hpack::status::list_too_large) {
            stream_error(host, sid, h2_error::enhance_your_calm);
            return;
        }
        if (trailers_) {
            trailers_ = false;
            end_of_request(sid, host);           // trailers are validated by HPACK and discarded
            return;
        }
        if (!build_request(*s)) {
            stream_error(host, sid, h2_error::protocol_error);
            return;
        }
        s->have_headers = true;
        if (block_end_stream_)
            end_of_request(sid, host);
    }

    // RFC 9113 §8.3: what makes a request malformed. Every one of these is a
    // request-smuggling vector when an h2 front end talks HTTP/1.1 to a back
    // end, which is why they are refused here rather than normalised.
    bool build_request(detail::h2_stream& s)
    {
        std::string method, scheme, path, authority;
        bool have_method = false, have_scheme = false, have_path = false, have_authority = false;
        bool seen_regular = false;
        std::string cookie;
        bool have_cookie = false;

        request& req = s.req;
        req.clear();
        req.http_major = 2;
        req.http_minor = 0;
        req.keep_alive = true;

        for (const auto& f : fields_) {
            const std::string& n = f.first;
            const std::string& v = f.second;
            if (n.empty())
                return false;
            if (!valid_value(v))
                return false;

            if (n[0] == ':') {
                if (seen_regular)
                    return false;               // pseudo-headers come first, always
                if (n == ":method") {
                    if (have_method) return false;
                    have_method = true;
                    method = v;
                } else if (n == ":scheme") {
                    if (have_scheme) return false;
                    have_scheme = true;
                    scheme = v;
                } else if (n == ":path") {
                    if (have_path) return false;
                    have_path = true;
                    path = v;
                } else if (n == ":authority") {
                    if (have_authority) return false;
                    have_authority = true;
                    authority = v;
                } else {
                    return false;               // ":status" or anything unknown
                }
                continue;
            }

            seen_regular = true;
            if (!valid_name(n))
                return false;
            // §8.2.2: these have no meaning without HTTP/1.1's connection
            // model, and tolerating them is how a desync gets built
            if (n == "connection" || n == "keep-alive" || n == "proxy-connection" ||
                n == "transfer-encoding" || n == "upgrade")
                return false;
            if (n == "te" && v != "trailers")
                return false;
            if (n == "cookie") {
                // §8.2.3: crumbs are rejoined so a handler sees one field
                if (have_cookie)
                    cookie += "; ";
                cookie += v;
                have_cookie = true;
                continue;
            }
            if (n == "content-length") {
                unsigned long long declared = 0;
                if (!parse_uint(v, declared))
                    return false;
                if (s.has_content_length && s.content_length != declared)
                    return false;
                s.has_content_length = true;
                s.content_length = declared;
            }
            req.headers.add(n, v);
        }

        if (have_cookie)
            req.headers.add("cookie", cookie);

        if (!have_method || method.empty())
            return false;
        if (method == "CONNECT") {
            // The CONNECT form has no :scheme and no :path, and we do not
            // proxy - refuse it rather than half-support it
            return false;
        }
        if (!have_scheme || scheme.empty())
            return false;
        if (!have_path || path.empty())
            return false;

        req.method_text = method;
        req.method = parse_method(method);
        s.head_request = (req.method == method::head);
        req.target = path;

        if (path == "*") {
            req.path = "*";
        } else {
            if (path[0] != '/')
                return false;
            const std::size_t q = path.find('?');
            const std::size_t plen = (q == std::string::npos) ? path.size() : q;
            if (q != std::string::npos)
                req.query.assign(path, q + 1, path.size() - q - 1);
            if (!detail::percent_decode(path.data(), plen, req.path, false))
                return false;
            if (req.path.find('\0') != std::string::npos)
                return false;
        }
        // Handlers written against HTTP/1.1 read Host; :authority is the same
        // information under a new name, so give them both
        if (have_authority && !authority.empty() && !req.headers.has("host"))
            req.headers.add("host", authority);
        return true;
    }

    static bool valid_name(const std::string& n) noexcept
    {
        for (char c : n) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (u >= 'A' && u <= 'Z')           // §8.2.1: field names are lowercase on the wire
                return false;
            if (u <= 0x20 || u == 0x7f || u == ':')
                return false;
            if (!detail::is_token_char(c))
                return false;
        }
        return !n.empty();
    }

    static bool valid_value(const std::string& v) noexcept
    {
        for (char c : v) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (u == 0x00 || u == 0x0a || u == 0x0d)
                return false;
        }
        if (!v.empty() && (v.front() == ' ' || v.front() == '\t' ||
                           v.back() == ' ' || v.back() == '\t'))
            return false;
        return true;
    }

    static bool parse_uint(const std::string& v, unsigned long long& out) noexcept
    {
        if (v.empty())
            return false;
        unsigned long long n = 0;
        for (char c : v) {
            if (!detail::is_digit(c))
                return false;
            if (n > (~0ull - 9) / 10)
                return false;
            n = n * 10 + unsigned(c - '0');
        }
        out = n;
        return true;
    }

    void end_of_request(std::uint32_t sid, connection_host& host)
    {
        // A strong reference for the whole call: everything below can end up
        // erasing this stream from the map, including the handler answering
        // inline, and `req` is handed out by reference
        std::shared_ptr<detail::h2_stream> keep = hold(sid);
        if (!keep)
            return;
        detail::h2_stream& s = *keep;

        if (s.state == detail::h2_stream_state::open)
            s.state = detail::h2_stream_state::half_closed_remote;
        else if (s.state == detail::h2_stream_state::half_closed_local)
            s.state = detail::h2_stream_state::closed;

        if (s.delivered)
            return;
        if (!s.have_headers) {
            stream_error(host, sid, h2_error::protocol_error);
            return;
        }
        // §8.1.2.6: a body that does not match its declared length is
        // malformed, and it is malformed at exactly the point where a proxy
        // would otherwise re-frame it wrongly
        if (s.has_content_length && s.content_length != s.body_bytes) {
            stream_error(host, sid, h2_error::protocol_error);
            return;
        }

        s.delivered = true;
        s.awaiting = true;
        host.deliver(s.req, sid);
        // The handler may have answered inline, in which case the stream is
        // already finished and may already be out of the map
        if (s.end_sent && !s.streaming &&
            s.state == detail::h2_stream_state::half_closed_remote)
            forget(sid);
    }

    // ------------------------------------------------------------ responses

    void send_response_headers(detail::h2_stream& s, const response& res, bool end_stream,
                               connection_host& host)
    {
        if (s.headers_sent)
            return;
        s.headers_sent = true;

        fields_out_.clear();
        char code[8];
        const int status = (res.status >= 100 && res.status <= 599) ? res.status : 500;
        std::snprintf(code, sizeof code, "%d", status);
        fields_out_.emplace_back(":status", code);

        bool have_date = false, have_server = false;
        for (const auto& h : res.headers) {
            std::string n = h.first;
            detail::ascii_lower(n);
            // Connection-specific fields have no meaning here and RFC 9113
            // §8.2.2 forbids sending them
            if (n == "connection" || n == "keep-alive" || n == "proxy-connection" ||
                n == "transfer-encoding" || n == "upgrade")
                continue;
            if (n == "date") have_date = true;
            if (n == "server") have_server = true;
            fields_out_.emplace_back(std::move(n), sanitise(h.second));
        }
        if (!have_date)
            fields_out_.emplace_back("date", host.http_date());
        if (!have_server)
            fields_out_.emplace_back("server", sanitise(host.config().server_name));

        std::string block;
        enc_.encode(fields_out_, block);

        // A header block larger than one frame becomes HEADERS plus
        // CONTINUATIONs; the peer's own max_frame_size decides where the cuts
        // fall, and nothing may be interleaved between them
        std::string o;
        const std::size_t chunk = peer_max_frame_;
        const std::size_t first = std::min(chunk, block.size());
        const bool one_frame = (first == block.size());
        put(o, std::uint32_t(first), h2_frame::headers,
            std::uint8_t((one_frame ? h2_flag_end_headers : 0) |
                         (end_stream ? h2_flag_end_stream : 0)),
            s.id);
        o.append(block, 0, first);
        std::size_t sent = first;
        while (sent < block.size()) {
            const std::size_t n = std::min(chunk, block.size() - sent);
            const bool last = (sent + n == block.size());
            put(o, std::uint32_t(n), h2_frame::continuation,
                std::uint8_t(last ? h2_flag_end_headers : 0), s.id);
            o.append(block, sent, n);
            sent += n;
        }
        emit(host, o);
    }

    static std::string sanitise(const std::string& v)
    {
        std::string o;
        o.reserve(v.size());
        for (char c : v)
            o.push_back((c == '\r' || c == '\n' || c == '\0') ? ' ' : c);
        // A leading or trailing space makes the field malformed on the wire
        std::size_t b = 0, e = o.size();
        while (b < e && (o[b] == ' ' || o[b] == '\t')) ++b;
        while (e > b && (o[e - 1] == ' ' || o[e - 1] == '\t')) --e;
        return o.substr(b, e - b);
    }

    // ---------------------------------------------------------------- state

    parse_limits lim_;
    h2_config cfg_;

    hpack::decoder dec_;
    hpack::encoder enc_;
    std::vector<hpack::field> fields_;
    std::vector<hpack::field> fields_out_;

    std::map<std::uint32_t, std::shared_ptr<detail::h2_stream>> streams_;
    std::deque<std::uint32_t> closed_;
    std::map<std::uint32_t, std::uint32_t> stream_updates_;

    std::string block_;                     // the header block being assembled
    std::uint32_t block_stream_ = 0;
    std::uint32_t cont_stream_ = 0;         // non-zero while CONTINUATIONs are owed
    bool block_end_stream_ = false;
    bool trailers_ = false;

    std::uint32_t last_peer_stream_ = 0;
    std::uint32_t peer_max_frame_ = 16384;
    std::uint32_t peer_initial_window_ = 65535;
    std::uint32_t peer_max_header_list_ = 0xffffffffu;

    std::int64_t conn_send_window_ = 65535;
    std::int64_t conn_recv_window_ = 65535;
    std::uint32_t conn_pending_update_ = 0;

    detail::h2_bucket rst_bucket_;
    detail::h2_bucket ctl_bucket_;

    std::size_t last_bytes_ = 0;
    bool preface_seen_ = false;
    bool settings_sent_ = false;
    bool first_frame_seen_ = false;
    bool goaway_sent_ = false;
    bool goaway_received_ = false;
    bool close_ = false;
    h2_error last_error_ = h2_error::no_error;
};

// ---------------------------------------------------------- protocol selection
//
// The delegate that makes one binary serve both. It owns the connection for
// exactly as long as it takes to decide - ALPN if the transport negotiated
// one, the client preface if it did not - and then hands over through the same
// switch_protocol path the WebSocket upgrade uses.

class alpn_protocol final : public protocol_delegate {
public:
    explicit alpn_protocol(parse_limits lim, h2_config cfg = h2_config{},
                           bool allow_prior_knowledge = true)
        : lim_(lim), cfg_(cfg), prior_knowledge_(allow_prior_knowledge) {}

    const char* name() const noexcept override { return "alpn"; }
    void respond(std::uint64_t, response&&, connection_host&) override {}
    bool close_after_flush() const noexcept override { return false; }
    bool busy() const noexcept override { return false; }
    bool receiving() const noexcept override { return false; }

    bool consume(std::string& in, connection_host& host) override
    {
        const char* negotiated = host.alpn();
        if (negotiated && std::strcmp(negotiated, "h2") == 0) {
            // ALPN said h2, so the preface is mandatory and http2_protocol
            // will reject anything else - no sniffing required
            host.switch_protocol(std::unique_ptr<protocol_delegate>(
                new http2_protocol(lim_, cfg_)));
            return true;
        }
        if (negotiated && *negotiated) {        // http/1.1, or anything else we were offered
            host.switch_protocol(std::unique_ptr<protocol_delegate>(new http1_protocol(lim_)));
            return true;
        }
        if (in.empty())
            return true;                        // nothing negotiated and nothing to look at yet

        if (prior_knowledge_) {
            const std::size_t have = std::min(in.size(), kH2PrefaceCommit);
            if (std::memcmp(in.data(), kH2Preface, have) == 0) {
                if (have < kH2PrefaceCommit)
                    return true;                // still could be either; wait for the rest
                // Committed. The remaining octets are http2_protocol's to
                // check, so a preface that goes wrong late is a GOAWAY.
                host.switch_protocol(std::unique_ptr<protocol_delegate>(
                    new http2_protocol(lim_, cfg_)));
                return true;
            }
        }
        host.switch_protocol(std::unique_ptr<protocol_delegate>(new http1_protocol(lim_)));
        return true;
    }

private:
    parse_limits lim_;
    h2_config cfg_;
    bool prior_knowledge_;
};

// Serve HTTP/2 as well as HTTP/1.1 on this server, chosen per connection.
//
//   over TLS   the transport's ALPN decides; offer {"h2", "http/1.1"} there
//   plaintext  a client that opens with the h2 preface gets h2c, everyone
//              else gets HTTP/1.1
//
// Upgrade-based h2c (the `Upgrade: h2c` header on an HTTP/1.1 request) is
// deliberately not implemented: RFC 9113 removed it, no browser ever shipped
// it, and it is the one h2 entry point that has to reconstruct a request from
// a base64 SETTINGS payload - risk with no callers.
inline void enable_http2(server& srv, h2_config cfg = h2_config{},
                         bool allow_prior_knowledge = true)
{
    srv.protocol_factory([cfg, allow_prior_knowledge](const server_config& sc) {
        return std::unique_ptr<protocol_delegate>(
            new alpn_protocol(sc.limits, cfg, allow_prior_knowledge));
    });
}

// Speak HTTP/2 and nothing else: every connection is h2 from its first octet,
// and an HTTP/1.1 request gets a GOAWAY rather than a reply.
//
// This is the right mode behind a TLS-terminating proxy that has already done
// the ALPN, and it is also the mode h2spec's cleartext run assumes - which is
// worth stating plainly, because it is the one place the two modes cannot
// agree. Given "INVALID CONNECTION PREFACE" on a mixed listener, the honest
// answer is an HTTP/1.1 400: those bytes are a malformed HTTP/1.1 request, and
// the only reason to call them a malformed HTTP/2 preface instead is that we
// guessed. An h2-only listener has no guess to make, so it answers GOAWAY.
inline void enable_http2_only(server& srv, h2_config cfg = h2_config{})
{
    srv.protocol_factory([cfg](const server_config& sc) {
        return std::unique_ptr<protocol_delegate>(new http2_protocol(sc.limits, cfg));
    });
}

} // namespace http
} // namespace snicholls

#endif // SNICHOLLS_HAS_HTTP_SERVER
#endif /* http2_hpp */
