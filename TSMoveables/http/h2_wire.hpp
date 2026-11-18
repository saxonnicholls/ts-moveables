//
//  h2_wire.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - what both ends of an HTTP/2 connection share:
//  HPACK (RFC 7541), the frame types, flags, error codes and settings of RFC
//  9113, the frame header, the field-validity rules, and the token bucket the
//  control-frame limits use. The server (http2.hpp) and the client
//  (h2_client.hpp) are each a state machine over these; nothing here knows
//  which side it is on, does I/O, or needs more than the standard library.
//
//  Split out of http2.hpp when the client arrived, so the codec the RFC 7541
//  Appendix C vectors grade is the one code both directions run.
//

#ifndef ts_moveables_http_h2_wire_hpp
#define ts_moveables_http_h2_wire_hpp

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "message.hpp"

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

// ------------------------------------------------------------ frame plumbing

namespace detail {

// RFC 9113 §4.1: the nine-octet header every frame starts with
struct h2_frame_header {
    std::uint32_t length = 0;
    std::uint8_t type = 0;
    std::uint8_t flags = 0;
    std::uint32_t stream = 0;
};

// The reserved high bit of the stream id is ignored on receipt, not
// validated: §4.1 is explicit about it
inline h2_frame_header h2_read_header(const unsigned char* p) noexcept
{
    h2_frame_header h;
    h.length = (std::uint32_t(p[0]) << 16) | (std::uint32_t(p[1]) << 8) | std::uint32_t(p[2]);
    h.type = p[3];
    h.flags = p[4];
    h.stream = ((std::uint32_t(p[5]) & 0x7fu) << 24) | (std::uint32_t(p[6]) << 16) |
               (std::uint32_t(p[7]) << 8) | std::uint32_t(p[8]);
    return h;
}

inline void h2_put_header(std::string& o, std::uint32_t len, h2_frame type, std::uint8_t flags,
                          std::uint32_t sid)
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

inline void h2_put32(std::string& o, std::uint32_t v)
{
    o.push_back(char((v >> 24) & 0xff));
    o.push_back(char((v >> 16) & 0xff));
    o.push_back(char((v >> 8) & 0xff));
    o.push_back(char(v & 0xff));
}

inline std::uint32_t h2_get32(const unsigned char* p) noexcept
{
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}

inline void h2_put_window_update(std::string& o, std::uint32_t sid, std::uint32_t inc)
{
    h2_put_header(o, 4, h2_frame::window_update, 0, sid);
    h2_put32(o, inc);
}

inline void h2_put_rst(std::string& o, std::uint32_t sid, h2_error code)
{
    h2_put_header(o, 4, h2_frame::rst_stream, 0, sid);
    h2_put32(o, std::uint32_t(code));
}

inline void h2_put_goaway(std::string& o, std::uint32_t last_stream, h2_error code, const char* why)
{
    const std::size_t why_len = why ? std::strlen(why) : 0;
    h2_put_header(o, std::uint32_t(8 + why_len), h2_frame::goaway, 0, 0);
    h2_put32(o, last_stream);
    h2_put32(o, std::uint32_t(code));
    if (why_len)
        o.append(why, why_len);
}

// A header block as HEADERS plus CONTINUATIONs, cut at the peer's own
// SETTINGS_MAX_FRAME_SIZE; nothing may be interleaved between them (§6.10)
inline void h2_put_header_block(std::string& o, const std::string& block, std::size_t max_frame,
                                bool end_stream, std::uint32_t sid)
{
    const std::size_t first = std::min(max_frame, block.size());
    const bool one_frame = (first == block.size());
    h2_put_header(o, std::uint32_t(first), h2_frame::headers,
                  std::uint8_t((one_frame ? h2_flag_end_headers : 0) | (end_stream ? h2_flag_end_stream : 0)), sid);
    o.append(block, 0, first);
    std::size_t sent = first;
    while (sent < block.size()) {
        const std::size_t n = std::min(max_frame, block.size() - sent);
        const bool last = (sent + n == block.size());
        h2_put_header(o, std::uint32_t(n), h2_frame::continuation, std::uint8_t(last ? h2_flag_end_headers : 0), sid);
        o.append(block, sent, n);
        sent += n;
    }
}

// ------------------------------------------------------------- field rules

// §8.2.1: field names are lowercase tokens on the wire
inline bool h2_valid_name(const std::string& n) noexcept
{
    for (char c : n) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 'A' && u <= 'Z')
            return false;
        if (u <= 0x20 || u == 0x7f || u == ':')
            return false;
        if (!is_token_char(c))
            return false;
    }
    return !n.empty();
}

// §8.2.1: no NUL, CR or LF anywhere, and no whitespace at either end
inline bool h2_valid_value(const std::string& v) noexcept
{
    for (char c : v) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u == 0x00 || u == 0x0a || u == 0x0d)
            return false;
    }
    if (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.back() == ' ' || v.back() == '\t'))
        return false;
    return true;
}

// §8.2.2: fields with no meaning without HTTP/1.1's connection model, which
// neither side may send; tolerating them is how a desync gets built
inline bool h2_connection_specific(const std::string& lower_name) noexcept
{
    return lower_name == "connection" || lower_name == "keep-alive" || lower_name == "proxy-connection" ||
           lower_name == "transfer-encoding" || lower_name == "upgrade";
}

inline bool h2_parse_uint(const std::string& v, unsigned long long& out) noexcept
{
    if (v.empty())
        return false;
    unsigned long long n = 0;
    for (char c : v) {
        if (!is_digit(c))
            return false;
        if (n > (~0ull - 9) / 10)
            return false;
        n = n * 10 + unsigned(c - '0');
    }
    out = n;
    return true;
}

// A value made safe to send: CR, LF and NUL become spaces, and the ends are
// trimmed, since a leading or trailing space makes the field malformed
inline std::string h2_sanitise(const std::string& v)
{
    std::string o;
    o.reserve(v.size());
    for (char c : v)
        o.push_back((c == '\r' || c == '\n' || c == '\0') ? ' ' : c);
    std::size_t b = 0, e = o.size();
    while (b < e && (o[b] == ' ' || o[b] == '\t')) ++b;
    while (e > b && (o[e - 1] == ' ' || o[e - 1] == '\t')) --e;
    return o.substr(b, e - b);
}

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

} // namespace detail

} // namespace http
} // namespace snicholls

#endif // ts_moveables_http_h2_wire_hpp
