//
//  tests_http2.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - unit tests for HTTP/2 (RFC 9113) and HPACK (7541)
//
//  Two halves, and the first one matters most.
//
//  HPACK is checked against the *published* RFC 7541 Appendix C vectors -
//  every hex dump in C.2 through C.6, decoded and compared field by field,
//  with the dynamic table's size asserted after each block. That is the point:
//  a codec tested only by round-tripping through itself agrees with its own
//  bugs, and HPACK is exactly the kind of code where that agreement is
//  invisible until an unrelated client cannot talk to you. The C.5 and C.6
//  sequences run with a 256-octet table specifically because the RFC chose
//  that size to force evictions.
//
//  The second half drives a real server over loopback with a deliberately
//  literal HTTP/2 client - hand-built frames, no library shortcuts - and
//  covers framing, the stream state machine, flow control, multiplexed
//  out-of-order completion, and every abuse limit.
//

#include "test_helpers.hpp"

#include "../TSMoveables/http/http2.hpp"

#if SNICHOLLS_HAS_HTTP2

#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace snicholls;
using namespace snicholls::http;
using namespace std::chrono_literals;

namespace {

// ============================================================== HPACK helpers

std::string unhex(const std::string& h)
{
    std::string out;
    out.reserve(h.size() / 2);
    for (std::size_t i = 0; i + 1 < h.size(); i += 2) {
        auto v = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int hi = v(h[i]), lo = v(h[i + 1]);
        assert(hi >= 0 && lo >= 0);
        out.push_back(char((hi << 4) | lo));
    }
    return out;
}

hpack::status decode_hex(hpack::decoder& d, const std::string& hex,
                         std::vector<hpack::field>& out,
                         std::size_t max_list = 1u << 20, std::size_t max_fields = 1024)
{
    out.clear();
    const std::string bytes = unhex(hex);
    const unsigned char* p = reinterpret_cast<const unsigned char*>(bytes.data());
    return d.decode(p, p + bytes.size(), out, max_list, max_fields);
}

// The whole point of the exercise: the expected list comes from the RFC, not
// from anything this library computed
void expect_fields(const std::vector<hpack::field>& got,
                   const std::vector<hpack::field>& want)
{
    assert(got.size() == want.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        assert(got[i].first == want[i].first);
        assert(got[i].second == want[i].second);
    }
}

// ================================================== HPACK: integer primitives

void test_hpack_integers()
{
    // RFC 7541 C.1.1 - 10 in a 5-bit prefix is one octet
    {
        std::string o;
        hpack::write_int(o, 5, 0x00, 10);
        assert(o.size() == 1);
        assert(static_cast<unsigned char>(o[0]) == 0x0a);
        const unsigned char* p = reinterpret_cast<const unsigned char*>(o.data());
        std::uint64_t v = 0;
        assert(hpack::read_int(p, p + o.size(), 5, v));
        assert(v == 10);
    }
    // C.1.2 - 1337 in a 5-bit prefix spills into two continuation octets
    {
        std::string o;
        hpack::write_int(o, 5, 0x00, 1337);
        assert(o.size() == 3);
        assert(static_cast<unsigned char>(o[0]) == 0x1f);
        assert(static_cast<unsigned char>(o[1]) == 0x9a);
        assert(static_cast<unsigned char>(o[2]) == 0x0a);
        const unsigned char* p = reinterpret_cast<const unsigned char*>(o.data());
        std::uint64_t v = 0;
        assert(hpack::read_int(p, p + o.size(), 5, v));
        assert(v == 1337);
    }
    // C.1.3 - 42 starting at an octet boundary is 42
    {
        std::string o;
        hpack::write_int(o, 8, 0x00, 42);
        assert(o.size() == 1);
        assert(static_cast<unsigned char>(o[0]) == 0x2a);
    }
    // Round trip across the prefix boundaries that actually occur
    for (unsigned bits = 4; bits <= 7; ++bits) {
        for (std::uint64_t v : {0ull, 1ull, 14ull, 15ull, 16ull, 126ull, 127ull, 128ull,
                                255ull, 256ull, 16383ull, 16384ull, 1000000ull}) {
            std::string o;
            hpack::write_int(o, bits, 0x00, v);
            const unsigned char* p = reinterpret_cast<const unsigned char*>(o.data());
            std::uint64_t back = 0;
            assert(hpack::read_int(p, p + o.size(), bits, back));
            assert(back == v);
            assert(p == reinterpret_cast<const unsigned char*>(o.data()) + o.size());
        }
    }

    // A truncated continuation is a decoding error, not a partial value
    {
        const unsigned char trunc[] = {0x1f, 0x9a};
        const unsigned char* p = trunc;
        std::uint64_t v = 0;
        assert(!hpack::read_int(p, trunc + 2, 5, v));
    }
    // An unbounded run of continuation octets costs the attacker one byte
    // each and must not cost us anything
    {
        std::string bomb;
        bomb.push_back(char(0x1f));
        for (int i = 0; i < 64; ++i)
            bomb.push_back(char(0x80));
        bomb.push_back('\0');
        const unsigned char* p = reinterpret_cast<const unsigned char*>(bomb.data());
        std::uint64_t v = 0;
        assert(!hpack::read_int(p, p + bomb.size(), 5, v));
    }

    pass("hpack: integer representation matches RFC 7541 C.1 and rejects overflow");
}

// ============================================================= HPACK: Huffman

void test_hpack_huffman()
{
    // Every octet value round trips, including the 28- and 30-bit codes that
    // never appear in real headers and are therefore never accidentally
    // exercised
    for (int i = 0; i < 256; ++i) {
        const char c = char(i);
        std::string enc;
        hpack::huff_encode(&c, 1, enc);
        assert(enc.size() == hpack::huff_encoded_size(&c, 1));
        std::string dec;
        assert(hpack::huff_decode(reinterpret_cast<const unsigned char*>(enc.data()),
                                  enc.size(), dec, 64));
        assert(dec.size() == 1 && dec[0] == c);
    }
    // And so does every octet in one long string, which is where a state
    // machine that mishandles a nibble boundary shows up
    {
        std::string all;
        for (int i = 0; i < 256; ++i)
            all.push_back(char(i));
        std::string enc;
        hpack::huff_encode(all.data(), all.size(), enc);
        std::string dec;
        assert(hpack::huff_decode(reinterpret_cast<const unsigned char*>(enc.data()),
                                  enc.size(), dec, 1024));
        assert(dec == all);
    }

    // The RFC's own worked example: "www.example.com" is 12 octets Huffman-coded
    {
        const std::string s = "www.example.com";
        std::string enc;
        hpack::huff_encode(s.data(), s.size(), enc);
        assert(enc == unhex("f1e3c2e5f23a6ba0ab90f4ff"));
    }
    {
        const std::string s = "no-cache";
        std::string enc;
        hpack::huff_encode(s.data(), s.size(), enc);
        assert(enc == unhex("a8eb10649cbf"));
    }
    {
        const std::string s = "custom-value";
        std::string enc;
        hpack::huff_encode(s.data(), s.size(), enc);
        assert(enc == unhex("25a849e95bb8e8b4bf"));
    }

    // §5.2: an encoded EOS symbol is a decoding error, never a 257th byte
    {
        // Thirty set bits is EOS; four octets of 0xff contains it
        const std::string eos = unhex("ffffffff");
        std::string dec;
        assert(!hpack::huff_decode(reinterpret_cast<const unsigned char*>(eos.data()),
                                   eos.size(), dec, 64));
    }
    // §5.2: padding must be the most significant bits of EOS and must be
    // shorter than eight bits. "0" is a five-bit code, so an octet of
    // 0x00 | three pad bits is legal and a whole extra octet is not.
    {
        std::string ok;
        hpack::huff_encode("0", 1, ok);
        assert(ok.size() == 1);
        std::string dec;
        assert(hpack::huff_decode(reinterpret_cast<const unsigned char*>(ok.data()),
                                  1, dec, 8));
        assert(dec == "0");

        const std::string over = ok + std::string(1, char(0xff));   // eight pad bits
        std::string bad;
        assert(!hpack::huff_decode(reinterpret_cast<const unsigned char*>(over.data()),
                                   over.size(), bad, 8));
    }
    // Padding that is not all ones is a decoding error even when it is short
    {
        const std::string bad_pad = unhex("00");     // "0" then three zero bits
        std::string dec;
        assert(!hpack::huff_decode(reinterpret_cast<const unsigned char*>(bad_pad.data()),
                                   1, dec, 8));
    }
    // The decompression bound is enforced while decoding, not after it
    {
        std::string enc;
        hpack::huff_encode("aaaaaaaaaaaaaaaa", 16, enc);
        std::string dec;
        assert(!hpack::huff_decode(reinterpret_cast<const unsigned char*>(enc.data()),
                                   enc.size(), dec, 4));
    }

    pass("hpack: Huffman round trips every octet and rejects EOS and bad padding");
}

// =============================================== HPACK: RFC 7541 Appendix C.2

void test_hpack_representations()
{
    {   // C.2.1 literal with incremental indexing, both names literal
        hpack::decoder d;
        std::vector<hpack::field> f;
        assert(decode_hex(d, "400a637573746f6d2d6b65790d637573746f6d2d686561646572", f) ==
               hpack::status::ok);
        expect_fields(f, {{"custom-key", "custom-header"}});
        assert(d.table().count() == 1);
        assert(d.table().size() == 55);
    }
    {   // C.2.2 literal without indexing, indexed name (:path)
        hpack::decoder d;
        std::vector<hpack::field> f;
        assert(decode_hex(d, "040c2f73616d706c652f70617468", f) == hpack::status::ok);
        expect_fields(f, {{":path", "/sample/path"}});
        assert(d.table().count() == 0);          // "without indexing" means exactly that
    }
    {   // C.2.3 never indexed
        hpack::decoder d;
        std::vector<hpack::field> f;
        assert(decode_hex(d, "100870617373776f726406736563726574", f) == hpack::status::ok);
        expect_fields(f, {{"password", "secret"}});
        assert(d.table().count() == 0);
    }
    {   // C.2.4 indexed header field - one octet for a whole field
        hpack::decoder d;
        std::vector<hpack::field> f;
        assert(decode_hex(d, "82", f) == hpack::status::ok);
        expect_fields(f, {{":method", "GET"}});
    }

    pass("hpack: RFC 7541 C.2 header field representations");
}

// =========================================== HPACK: C.3 and C.4 request series

void run_request_series(const char* first, const char* second, const char* third,
                        const char* what)
{
    // One decoder across all three blocks, because the dynamic table is
    // connection state and the whole exercise is whether it evolves correctly
    hpack::decoder d;
    std::vector<hpack::field> f;

    assert(decode_hex(d, first, f) == hpack::status::ok);
    expect_fields(f, {{":method", "GET"},
                      {":scheme", "http"},
                      {":path", "/"},
                      {":authority", "www.example.com"}});
    assert(d.table().count() == 1);
    assert(d.table().size() == 57);

    assert(decode_hex(d, second, f) == hpack::status::ok);
    expect_fields(f, {{":method", "GET"},
                      {":scheme", "http"},
                      {":path", "/"},
                      {":authority", "www.example.com"},
                      {"cache-control", "no-cache"}});
    assert(d.table().count() == 2);
    assert(d.table().size() == 110);

    assert(decode_hex(d, third, f) == hpack::status::ok);
    expect_fields(f, {{":method", "GET"},
                      {":scheme", "https"},
                      {":path", "/index.html"},
                      {":authority", "www.example.com"},
                      {"custom-key", "custom-value"}});
    assert(d.table().count() == 3);
    assert(d.table().size() == 164);

    pass(what);
}

void test_hpack_requests()
{
    run_request_series("828684410f7777772e6578616d706c652e636f6d",
                       "828684be58086e6f2d6361636865",
                       "828785bf400a637573746f6d2d6b65790c637573746f6d2d76616c7565",
                       "hpack: RFC 7541 C.3 request series, no Huffman");

    run_request_series("828684418cf1e3c2e5f23a6ba0ab90f4ff",
                       "828684be5886a8eb10649cbf",
                       "828785bf408825a849e95ba97d7f8925a849e95bb8e8b4bf",
                       "hpack: RFC 7541 C.4 request series, Huffman coded");
}

// ========================================== HPACK: C.5 and C.6 response series

void run_response_series(const char* first, const char* second, const char* third,
                         const char* what)
{
    // The RFC sets SETTINGS_HEADER_TABLE_SIZE to 256 for these, precisely so
    // that the third block forces evictions. A decoder with the eviction rule
    // wrong still passes C.3 and C.4 and fails here.
    hpack::decoder d;
    d.initial_table_size(256);
    std::vector<hpack::field> f;

    assert(decode_hex(d, first, f) == hpack::status::ok);
    expect_fields(f, {{":status", "302"},
                      {"cache-control", "private"},
                      {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
                      {"location", "https://www.example.com"}});
    assert(d.table().size() == 222);
    assert(d.table().count() == 4);

    assert(decode_hex(d, second, f) == hpack::status::ok);
    expect_fields(f, {{":status", "307"},
                      {"cache-control", "private"},
                      {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
                      {"location", "https://www.example.com"}});
    assert(d.table().size() == 222);
    assert(d.table().count() == 4);

    assert(decode_hex(d, third, f) == hpack::status::ok);
    expect_fields(f, {{":status", "200"},
                      {"cache-control", "private"},
                      {"date", "Mon, 21 Oct 2013 20:13:22 GMT"},
                      {"location", "https://www.example.com"},
                      {"content-encoding", "gzip"},
                      {"set-cookie",
                       "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1"}});
    assert(d.table().size() == 215);
    assert(d.table().count() == 3);

    pass(what);
}

void test_hpack_responses()
{
    run_response_series(
        "4803333032580770726976617465611d4d6f6e2c203231204f637420323031332032303a"
        "31333a323120474d546e1768747470733a2f2f7777772e6578616d706c652e636f6d",
        "4803333037c1c0bf",
        "88c1611d4d6f6e2c203231204f637420323031332032303a31333a323220474d54c05a04"
        "677a69707738666f6f3d4153444a4b48514b425a584f5157454f50495541585157454f49"
        "553b206d61782d6167653d333630303b2076657273696f6e3d31",
        "hpack: RFC 7541 C.5 response series with eviction, no Huffman");

    run_response_series(
        "488264025885aec3771a4b6196d07abe941054d444a8200595040b8166e082a62d1bff6e"
        "919d29ad171863c78f0b97c8e9ae82ae43d3",
        "4883640effc1c0bf",
        "88c16196d07abe941054d444a8200595040b8166e084a62d1bffc05a839bd9ab77ad94e7"
        "821dd7f2e6c7b335dfdfcd5b3960d5af27087f3672c1ab270fb5291f9587316065c003ed"
        "4ee5b1063d5007",
        "hpack: RFC 7541 C.6 response series with eviction, Huffman coded");
}

// ================================================== HPACK: tables and refusals

void test_hpack_dynamic_table()
{
    hpack::dynamic_table t;
    t.max_size(100);
    t.insert("a", "1");                     // 34
    t.insert("b", "2");                     // 34
    assert(t.count() == 2 && t.size() == 68);
    assert(t.at(0)->first == "b");          // newest first, always
    assert(t.at(1)->first == "a");

    t.insert("c", "3");                     // 102 > 100: evicts the oldest
    assert(t.count() == 2 && t.size() == 68);
    assert(t.at(0)->first == "c");
    assert(t.at(1)->first == "b");

    // §4.4: an entry larger than the whole table empties it and is not added.
    // That is a legitimate instruction, not an error - getting it wrong makes
    // every later index off by one.
    t.insert(std::string(200, 'x'), "y");
    assert(t.count() == 0 && t.size() == 0);

    // Shrinking evicts immediately, not lazily
    t.insert("d", "4");
    t.insert("e", "5");
    assert(t.count() == 2);
    t.max_size(34);
    assert(t.count() == 1 && t.at(0)->first == "e");
    t.max_size(0);
    assert(t.count() == 0);

    pass("hpack: dynamic table eviction, ordering and the oversized-entry rule");
}

void test_hpack_refusals()
{
    std::vector<hpack::field> f;
    {   // §6.1: index 0 is not a header field
        hpack::decoder d;
        assert(decode_hex(d, "80", f) == hpack::status::compression_error);
    }
    {   // An index past the end of static + dynamic
        hpack::decoder d;
        assert(decode_hex(d, "be", f) == hpack::status::compression_error);
    }
    {   // A literal whose indexed name is out of range
        hpack::decoder d;
        assert(decode_hex(d, "7f0a0576616c7565", f) == hpack::status::compression_error);
    }
    {   // Truncated string literal
        hpack::decoder d;
        assert(decode_hex(d, "400a637573746f6d", f) == hpack::status::compression_error);
    }
    {   // §6.3: a dynamic table size update above SETTINGS_HEADER_TABLE_SIZE
        hpack::decoder d;
        d.initial_table_size(256);
        assert(decode_hex(d, "3fe107", f) == hpack::status::compression_error);  // 4096
    }
    {   // §4.2: a size update may only appear at the start of a block
        hpack::decoder d;
        assert(decode_hex(d, "823f00", f) == hpack::status::compression_error);
    }
    {   // ... and at the start it is fine, twice over
        hpack::decoder d;
        assert(decode_hex(d, "20302082", f) == hpack::status::ok);
        expect_fields(f, {{":method", "GET"}});
    }

    // The HPACK bomb: a small block that decodes to an enormous header list.
    // The cap has to stop the *output*, and it must not stop the decode -
    // the dynamic table is shared state and skipping it desynchronises the
    // connection for good.
    {
        hpack::decoder d;
        std::string block;
        for (int i = 0; i < 40; ++i) {
            block.push_back(char(0x40));                 // literal, incremental indexing
            hpack::write_int(block, 7, 0x00, 4);
            block += "name";
            hpack::write_int(block, 7, 0x00, 40);
            block += std::string(40, 'v');
        }
        std::vector<hpack::field> got;
        const unsigned char* p = reinterpret_cast<const unsigned char*>(block.data());
        const auto st = d.decode(p, p + block.size(), got, /*max_list*/ 400, /*max_fields*/ 1024);
        assert(st == hpack::status::list_too_large);
        assert(got.size() < 40);                         // output was cut
        assert(d.table().count() == 40);                 // state was not
    }
    // The field-count cap does the same job for many tiny fields
    {
        hpack::decoder d;
        std::string block;
        for (int i = 0; i < 50; ++i)
            block.push_back(char(0x82));                 // :method: GET, over and over
        std::vector<hpack::field> got;
        const unsigned char* p = reinterpret_cast<const unsigned char*>(block.data());
        assert(d.decode(p, p + block.size(), got, 1u << 20, 8) == hpack::status::list_too_large);
        assert(got.size() == 8);
    }

    pass("hpack: refuses bad indices, misplaced size updates and decompression bombs");
}

void test_hpack_encoder_round_trip()
{
    // Our encoder is static-table-only by design; what matters is that an
    // independent decoder - the one the RFC vectors validated - reads it back
    hpack::encoder e;
    hpack::decoder d;
    std::vector<hpack::field> in = {
        {":status", "200"},
        {":status", "404"},                          // an exact static hit further down
        {"content-type", "text/plain; charset=utf-8"},
        {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
        {"server", "ts-moveables"},
        {"x-custom-header", "a value with spaces and \"quotes\""},
        {"x-empty", ""},
        {"x-binary", std::string("\x01\x02\x7f\x80\xff", 5)},
    };
    std::string block;
    e.encode(in, block);

    std::vector<hpack::field> out;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(block.data());
    assert(d.decode(p, p + block.size(), out, 1u << 20, 1024) == hpack::status::ok);
    expect_fields(out, in);
    assert(d.table().count() == 0);                  // the encoder never indexes

    // A peer that shrinks the table gets told about it, once (RFC 7541 §4.2)
    {
        hpack::encoder e2;
        e2.peer_table_size(0);
        std::string b1, b2;
        e2.encode({{":status", "200"}}, b1);
        e2.encode({{":status", "200"}}, b2);
        assert(b1.size() == b2.size() + 1);
        assert(static_cast<unsigned char>(b1[0]) == 0x20);
    }

    pass("hpack: encoder output decodes back through the RFC-checked decoder");
}

// ============================================================== HTTP/2 client
//
// Deliberately literal: frames are built by hand so that what is under test is
// the server, not a shared helper that could be wrong in the same direction.

struct h2_in {
    std::uint32_t len = 0;
    std::uint8_t type = 0;
    std::uint8_t flags = 0;
    std::uint32_t sid = 0;
    std::string payload;
};

std::string raw_literal_block(const std::vector<hpack::field>& fields)
{
    // Literal without indexing, literal name: no static table, no dynamic
    // table, nothing normalised on the way out. That is what lets a test send
    // an uppercase field name or a duplicate pseudo-header on purpose.
    std::string o;
    for (const auto& f : fields) {
        o.push_back('\0');
        hpack::write_int(o, 7, 0x00, f.first.size());
        o += f.first;
        hpack::write_int(o, 7, 0x00, f.second.size());
        o += f.second;
    }
    return o;
}

class h2_client {
public:
    explicit h2_client(std::uint16_t port)
    {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        assert(fd_ >= 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        assert(::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
        timeval tv{};
        tv.tv_sec = 5;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        // Half of these tests deliberately provoke the server into closing,
        // and a write into the wreckage must be an error rather than a signal
        http::detail::suppress_sigpipe(fd_);
    }
    h2_client(const h2_client&) = delete;
    h2_client& operator=(const h2_client&) = delete;
    ~h2_client() { close(); }

    void close()
    {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    void write(const char* p, std::size_t n)
    {
        std::size_t off = 0;
        while (off < n) {
            const ssize_t k = ::send(fd_, p + off, n - off, http::detail::send_flags());
            if (k <= 0)
                return;                         // the peer went away: the test will say so
            off += std::size_t(k);
        }
    }
    void write(const std::string& s) { write(s.data(), s.size()); }

    void frame(std::uint8_t type, std::uint8_t flags, std::uint32_t sid,
               const std::string& payload)
    {
        std::string o;
        const std::uint32_t n = std::uint32_t(payload.size());
        o.push_back(char((n >> 16) & 0xff));
        o.push_back(char((n >> 8) & 0xff));
        o.push_back(char(n & 0xff));
        o.push_back(char(type));
        o.push_back(char(flags));
        o.push_back(char((sid >> 24) & 0x7f));
        o.push_back(char((sid >> 16) & 0xff));
        o.push_back(char((sid >> 8) & 0xff));
        o.push_back(char(sid & 0xff));
        o += payload;
        write(o);
    }

    void preface(const std::vector<std::pair<std::uint16_t, std::uint32_t>>& settings = {})
    {
        write(kH2Preface, kH2PrefaceLen);
        std::string s;
        for (const auto& kv : settings) {
            s.push_back(char((kv.first >> 8) & 0xff));
            s.push_back(char(kv.first & 0xff));
            s.push_back(char((kv.second >> 24) & 0xff));
            s.push_back(char((kv.second >> 16) & 0xff));
            s.push_back(char((kv.second >> 8) & 0xff));
            s.push_back(char(kv.second & 0xff));
        }
        frame(std::uint8_t(h2_frame::settings), 0, 0, s);
    }

    void request(std::uint32_t sid, const std::vector<hpack::field>& fields,
                 bool end_stream = true)
    {
        frame(std::uint8_t(h2_frame::headers),
              std::uint8_t(h2_flag_end_headers | (end_stream ? h2_flag_end_stream : 0)),
              sid, raw_literal_block(fields));
    }

    static std::vector<hpack::field> get(const char* path, const char* authority = "t")
    {
        return {{":method", "GET"}, {":scheme", "http"},
                {":path", path}, {":authority", authority}};
    }

    void window_update(std::uint32_t sid, std::uint32_t inc)
    {
        std::string p;
        p.push_back(char((inc >> 24) & 0xff));
        p.push_back(char((inc >> 16) & 0xff));
        p.push_back(char((inc >> 8) & 0xff));
        p.push_back(char(inc & 0xff));
        frame(std::uint8_t(h2_frame::window_update), 0, sid, p);
    }

    // Grant the window back as DATA is consumed, the way a real client does.
    // Off by default and deliberately so: the flow-control tests assert that
    // the server *stops* at the window it was given, and a client quietly
    // topping it up would make that assertion vacuous.
    void auto_window(bool on) { auto_window_ = on; }

    void rst(std::uint32_t sid, std::uint32_t code)
    {
        std::string p;
        p.push_back(char((code >> 24) & 0xff));
        p.push_back(char((code >> 16) & 0xff));
        p.push_back(char((code >> 8) & 0xff));
        p.push_back(char(code & 0xff));
        frame(std::uint8_t(h2_frame::rst_stream), 0, sid, p);
    }

    bool next(h2_in& f)
    {
        for (;;) {
            if (buf_.size() >= 9) {
                const unsigned char* p = reinterpret_cast<const unsigned char*>(buf_.data());
                const std::uint32_t len = (std::uint32_t(p[0]) << 16) |
                                          (std::uint32_t(p[1]) << 8) | p[2];
                if (buf_.size() >= 9u + len) {
                    f.len = len;
                    f.type = p[3];
                    f.flags = p[4];
                    f.sid = ((std::uint32_t(p[5]) & 0x7fu) << 24) |
                            (std::uint32_t(p[6]) << 16) |
                            (std::uint32_t(p[7]) << 8) | p[8];
                    f.payload.assign(buf_, 9, len);
                    buf_.erase(0, 9u + len);
                    note(f);
                    return true;
                }
            }
            char tmp[65536];
            const ssize_t n = ::recv(fd_, tmp, sizeof tmp, 0);
            if (n <= 0)
                return false;
            buf_.append(tmp, std::size_t(n));
        }
    }

    // Send a request body while honouring the windows the server granted.
    // Written the tedious way on purpose: it is the only way to prove the
    // server actually issues WINDOW_UPDATEs, because a client that ignores
    // flow control would deadlock against a correct server and pass against
    // one that never replenished at all.
    bool send_body(std::uint32_t sid, const std::string& data, bool end_stream)
    {
        if (!stream_win_.count(sid))
            stream_win_[sid] = std::int64_t(peer_initial_window_);
        std::size_t off = 0;
        while (off < data.size()) {
            while (stream_win_[sid] <= 0 || conn_win_ <= 0) {
                h2_in f;
                if (!next(f))
                    return false;               // the server stopped replenishing
            }
            const std::int64_t room = std::min(stream_win_[sid], conn_win_);
            std::size_t n = std::min<std::size_t>(data.size() - off, std::size_t(room));
            n = std::min<std::size_t>(n, 16384);
            const bool last = (off + n == data.size());
            frame(std::uint8_t(h2_frame::data),
                  std::uint8_t(last && end_stream ? h2_flag_end_stream : 0), sid,
                  data.substr(off, n));
            off += n;
            stream_win_[sid] -= std::int64_t(n);
            conn_win_ -= std::int64_t(n);
        }
        return true;
    }

    // Everything the peer sends until it closes - the HTTP/1.1 comparison path
    std::string read_all()
    {
        std::string out = buf_;
        buf_.clear();
        char tmp[8192];
        for (;;) {
            const ssize_t n = ::recv(fd_, tmp, sizeof tmp, 0);
            if (n <= 0)
                return out;
            out.append(tmp, std::size_t(n));
        }
    }

    // Collects one exchange: the response for `sid`, or the error that
    // replaced it. Everything else on the connection is drained past.
    struct result {
        int status = 0;
        std::vector<hpack::field> fields;
        std::string body;
        bool ended = false;
        bool reset = false;
        std::uint32_t reset_code = 0;
        bool goaway = false;
        std::uint32_t goaway_code = 0;
        int data_frames = 0;
    };

    // Frames are filed by stream as they arrive, not matched against the one
    // stream the caller happens to be asking about. That is not tidiness: a
    // multiplexed server answers in whatever order its handlers finish, so a
    // client that discarded the streams it was not currently waiting for
    // would fail against a *correct* server and pass against one that had
    // quietly serialised everything.
    bool collect(std::uint32_t sid, result& r)
    {
        for (;;) {
            auto it = results_.find(sid);
            if (it != results_.end() &&
                (it->second.ended || it->second.reset || it->second.goaway)) {
                r = it->second;
                results_.erase(it);
                return true;
            }
            if (goaway_) {
                r.goaway = true;
                r.goaway_code = goaway_code_;
                return true;
            }
            h2_in f;
            if (!next(f))
                return false;
            ingest(f);
        }
    }

    // Drains until a GOAWAY arrives; the connection-error path
    bool wait_goaway(std::uint32_t& code)
    {
        h2_in f;
        while (next(f)) {
            if (f.type == std::uint8_t(h2_frame::goaway)) {
                code = (f.payload.size() >= 8) ? be32(f.payload.data() + 4) : 0xffffffffu;
                return true;
            }
        }
        return false;
    }

    // Reads frames of the given type until one arrives, or the peer goes away
    bool wait_type(std::uint8_t type, h2_in& f)
    {
        while (next(f))
            if (f.type == type)
                return true;
        return false;
    }

    static std::uint32_t be32(const char* p)
    {
        const unsigned char* u = reinterpret_cast<const unsigned char*>(p);
        return (std::uint32_t(u[0]) << 24) | (std::uint32_t(u[1]) << 16) |
               (std::uint32_t(u[2]) << 8) | u[3];
    }

private:
    void ingest(const h2_in& f)
    {
        if (f.type == std::uint8_t(h2_frame::goaway)) {
            goaway_ = true;
            goaway_code_ = (f.payload.size() >= 8) ? be32(f.payload.data() + 4) : 0xffffffffu;
            return;
        }
        if (f.sid == 0)
            return;
        result& r = results_[f.sid];
        if (f.type == std::uint8_t(h2_frame::rst_stream)) {
            r.reset = true;
            if (f.payload.size() >= 4)
                r.reset_code = be32(f.payload.data());
            return;
        }
        if (f.type == std::uint8_t(h2_frame::headers) ||
            f.type == std::uint8_t(h2_frame::continuation)) {
            // One buffer is enough: HTTP/2 forbids interleaving header blocks
            block_ += f.payload;
            if (f.flags & h2_flag_end_headers) {
                const unsigned char* p = reinterpret_cast<const unsigned char*>(block_.data());
                r.fields.clear();
                assert(dec_.decode(p, p + block_.size(), r.fields, 1u << 20, 1024) ==
                       hpack::status::ok);
                block_.clear();
                for (const auto& kv : r.fields)
                    if (kv.first == ":status")
                        r.status = std::atoi(kv.second.c_str());
            }
        } else if (f.type == std::uint8_t(h2_frame::data)) {
            r.body += f.payload;
            ++r.data_frames;
            if (auto_window_ && f.len) {
                window_update(f.sid, f.len);
                window_update(0, f.len);
            }
        }
        if ((f.flags & h2_flag_end_stream) &&
            (f.type == std::uint8_t(h2_frame::data) ||
             f.type == std::uint8_t(h2_frame::headers)))
            r.ended = true;
    }

    // Track what the server grants us, so send_body() can be honest about it
    void note(const h2_in& f)
    {
        if (f.type == std::uint8_t(h2_frame::window_update) && f.payload.size() >= 4) {
            const std::int64_t inc = std::int64_t(be32(f.payload.data()) & 0x7fffffffu);
            if (f.sid == 0)
                conn_win_ += inc;
            else
                stream_win_[f.sid] += inc;
        } else if (f.type == std::uint8_t(h2_frame::settings) && !(f.flags & h2_flag_ack)) {
            for (std::size_t i = 0; i + 6 <= f.payload.size(); i += 6) {
                const unsigned char* u =
                    reinterpret_cast<const unsigned char*>(f.payload.data()) + i;
                const std::uint16_t id = std::uint16_t((std::uint32_t(u[0]) << 8) | u[1]);
                if (id == h2_settings_initial_window_size)
                    peer_initial_window_ = be32(f.payload.data() + i + 2);
            }
        }
    }

    int fd_ = -1;
    std::string buf_;
    std::string block_;
    hpack::decoder dec_;
    std::int64_t conn_win_ = 65535;
    std::uint32_t peer_initial_window_ = 65535;
    std::map<std::uint32_t, std::int64_t> stream_win_;
    std::map<std::uint32_t, result> results_;
    bool goaway_ = false;
    bool auto_window_ = false;
    std::uint32_t goaway_code_ = 0;
};

// A server on its own thread with h2 enabled, torn down cleanly
class h2_server {
public:
    explicit h2_server(h2_config h2 = h2_config{}, server_config cfg = server_config{},
                       bool only_h2 = false)
        : srv(std::move(cfg))
    {
        if (only_h2)
            enable_http2_only(srv, h2);
        else
            enable_http2(srv, h2);
    }

    void start()
    {
        port = srv.listen("127.0.0.1", 0);
        assert(port != 0);
        th = std::thread([this] { srv.run(); });
        spin_until([this] { return srv.running(); });
    }

    ~h2_server()
    {
        srv.stop();
        if (th.joinable())
            th.join();
    }

    server srv;
    std::uint16_t port = 0;
    std::thread th;
};

void add_common_routes(server& s)
{
    s.get("/hello", [](const request& req, responder r) {
        r.send(200, "text/plain", "hello " + req.header_or("host", "?"));
    });
    // A real 204: send_status() attaches a reason-phrase body, which is fine
    // on HTTP/1.1 and wrong for a status defined to have no content
    s.get("/empty", [](const request&, responder r) { r.send(response(204)); });
    s.post("/echo", [](const request& req, responder r) {
        r.send(200, "application/octet-stream", req.body);
    });
    s.get("/big", [](const request&, responder r) {
        r.send(200, "text/plain", std::string(100, 'z'));
    });
    s.head("/hello", [](const request&, responder r) {
        r.send(200, "text/plain", "hello");
    });
}

// ================================================= HTTP/2: the happy path

void test_h2c_prior_knowledge()
{
    h2_server s;
    add_common_routes(s.srv);
    s.start();

    h2_client c(s.port);
    c.preface();

    // The server preface is a SETTINGS frame, before anything else
    h2_in f;
    assert(c.next(f));
    assert(f.type == std::uint8_t(h2_frame::settings));
    assert(f.sid == 0);
    assert((f.flags & h2_flag_ack) == 0);
    assert(f.len % 6 == 0);

    c.request(1, h2_client::get("/hello", "example.test"));
    h2_client::result r;
    assert(c.collect(1, r));
    assert(r.ended && !r.reset && !r.goaway);
    assert(r.status == 200);
    assert(r.body == "hello example.test");     // :authority reaches a Host-reading handler

    // Field names arrive lowercase, and the connection-specific ones are gone
    bool saw_date = false, saw_server = false, saw_len = false;
    for (const auto& kv : r.fields) {
        for (char ch : kv.first)
            assert(!(ch >= 'A' && ch <= 'Z'));
        assert(kv.first != "connection" && kv.first != "transfer-encoding");
        if (kv.first == "date") saw_date = true;
        if (kv.first == "server") saw_server = true;
        if (kv.first == "content-length") saw_len = true;
    }
    assert(saw_date && saw_server && saw_len);

    // A second request on the same connection, and a 204 with no body
    c.request(3, h2_client::get("/empty"));
    h2_client::result r2;
    assert(c.collect(3, r2));
    assert(r2.status == 204 && r2.body.empty() && r2.ended);

    pass("http2: h2c with prior knowledge serves requests on one connection");
}

void test_h2_request_body_and_head()
{
    h2_server s;
    add_common_routes(s.srv);
    s.start();

    h2_client c(s.port);
    c.preface();

    {   // A POST split across two DATA frames, with a matching content-length
        std::vector<hpack::field> f = {{":method", "POST"}, {":scheme", "http"},
                                       {":path", "/echo"}, {":authority", "t"},
                                       {"content-length", "9"}};
        c.frame(std::uint8_t(h2_frame::headers), h2_flag_end_headers, 1,
                raw_literal_block(f));
        c.frame(std::uint8_t(h2_frame::data), 0, 1, "abcde");
        c.frame(std::uint8_t(h2_frame::data), h2_flag_end_stream, 1, "fghi");

        h2_client::result r;
        assert(c.collect(1, r));
        assert(r.status == 200 && r.body == "abcdefghi");
    }
    {   // A HEAD response carries the length and no DATA at all
        std::vector<hpack::field> f = {{":method", "HEAD"}, {":scheme", "http"},
                                       {":path", "/hello"}, {":authority", "t"}};
        c.request(3, f);
        h2_client::result r;
        assert(c.collect(3, r));
        assert(r.status == 200);
        assert(r.body.empty());
        assert(r.data_frames == 0);
    }
    {   // §8.1.2.6: a body shorter than its declared length is malformed
        std::vector<hpack::field> f = {{":method", "POST"}, {":scheme", "http"},
                                       {":path", "/echo"}, {":authority", "t"},
                                       {"content-length", "10"}};
        c.frame(std::uint8_t(h2_frame::headers), h2_flag_end_headers, 5,
                raw_literal_block(f));
        c.frame(std::uint8_t(h2_frame::data), h2_flag_end_stream, 5, "short");
        h2_client::result r;
        assert(c.collect(5, r));
        assert(r.reset && r.reset_code == std::uint32_t(h2_error::protocol_error));
    }

    pass("http2: request bodies, HEAD, and content-length mismatch");
}

void test_h2_multiplexing()
{
    h2_server s;
    // Three concurrent requests, answered in the reverse order from worker
    // threads: this is the design's whole claim about async responders, and
    // multiplexing is where it stops being decorative
    std::atomic<int> parked{0};
    static std::vector<responder> held;
    static std::mutex held_mu;

    s.srv.get("/slow/:n", [&parked](const request&, responder r) {
        {
            std::lock_guard<std::mutex> g(held_mu);
            held.push_back(std::move(r));
        }
        parked.fetch_add(1, std::memory_order_release);
    });
    s.start();

    h2_client c(s.port);
    c.preface();
    c.request(1, h2_client::get("/slow/1"));
    c.request(3, h2_client::get("/slow/2"));
    c.request(5, h2_client::get("/slow/3"));

    assert(spin_until_for([&parked] { return parked.load(std::memory_order_acquire) == 3; }));

    // Answer newest first, from a thread that is not the loop's
    std::thread worker([] {
        std::vector<responder> mine;
        {
            std::lock_guard<std::mutex> g(held_mu);
            mine = std::move(held);
            held.clear();
        }
        for (std::size_t i = mine.size(); i-- > 0;)
            mine[i].send(200, "text/plain", "stream" + std::to_string(mine[i].stream()));
    });
    worker.join();

    // Every stream is answered, whatever order the frames arrived in
    for (std::uint32_t sid : {1u, 3u, 5u}) {
        h2_client::result r;
        assert(c.collect(sid, r));
        assert(r.status == 200);
        assert(r.body == "stream" + std::to_string(sid));
    }

    {
        std::lock_guard<std::mutex> g(held_mu);
        held.clear();
    }
    pass("http2: three multiplexed streams complete out of order from a worker thread");
}

void test_h2_streamed_response()
{
    h2_server s;
    s.srv.get("/stream", [](const request&, responder r) {
        auto out = r.stream(response(200).type("text/plain"));
        for (int i = 0; i < 5; ++i)
            out.write("piece" + std::to_string(i) + ";");
        out.end();
    });
    s.start();

    h2_client c(s.port);
    c.preface();
    c.request(1, h2_client::get("/stream"));
    h2_client::result r;
    assert(c.collect(1, r));
    assert(r.status == 200);
    assert(r.body == "piece0;piece1;piece2;piece3;piece4;");
    assert(r.ended);
    // No Transfer-Encoding: h2 frames are the framing, and sending one would
    // be a protocol violation rather than a redundancy
    for (const auto& kv : r.fields)
        assert(kv.first != "transfer-encoding");

    pass("http2: response_stream maps onto DATA frames with no chunked framing");
}

// ================================================ HTTP/2: framing and refusals

void expect_connection_error(h2_client& c, h2_error want)
{
    std::uint32_t code = 0;
    assert(c.wait_goaway(code));
    assert(code == std::uint32_t(want));
}

void test_h2_bad_preface()
{
    h2_server s;
    add_common_routes(s.srv);
    s.start();
    {   // A preface that goes wrong halfway through
        h2_client c(s.port);
        c.write("PRI * HTTP/2.0\r\n\r\nXX\r\n\r\n");
        expect_connection_error(c, h2_error::protocol_error);
    }
    {   // A well-formed preface followed by something other than SETTINGS
        h2_client c(s.port);
        c.write(kH2Preface, kH2PrefaceLen);
        c.frame(std::uint8_t(h2_frame::ping), 0, 0, std::string(8, '\0'));
        expect_connection_error(c, h2_error::protocol_error);
    }
    pass("http2: an invalid connection preface is a connection error");
}

void test_h2_frame_refusals()
{
    // Each of these is its own connection: a connection error ends the
    // connection, which is the behaviour being asserted
    struct probe {
        const char* what;
        h2_error want;
        void (*send)(h2_client&);
    };

    static const probe probes[] = {
        {"DATA on stream 0", h2_error::protocol_error,
         [](h2_client& c) { c.frame(std::uint8_t(h2_frame::data), 0, 0, "x"); }},
        {"HEADERS on stream 0", h2_error::protocol_error,
         [](h2_client& c) { c.frame(std::uint8_t(h2_frame::headers), h2_flag_end_headers, 0, ""); }},
        {"even stream identifier", h2_error::protocol_error,
         [](h2_client& c) { c.request(2, h2_client::get("/hello")); }},
        {"PUSH_PROMISE from a client", h2_error::protocol_error,
         [](h2_client& c) { c.frame(std::uint8_t(h2_frame::push_promise), h2_flag_end_headers,
                                    1, std::string(4, '\0')); }},
        {"PING on a non-zero stream", h2_error::protocol_error,
         [](h2_client& c) { c.frame(std::uint8_t(h2_frame::ping), 0, 1, std::string(8, '\0')); }},
        {"PING with the wrong length", h2_error::frame_size_error,
         [](h2_client& c) { c.frame(std::uint8_t(h2_frame::ping), 0, 0, std::string(6, '\0')); }},
        {"SETTINGS on a non-zero stream", h2_error::protocol_error,
         [](h2_client& c) { c.frame(std::uint8_t(h2_frame::settings), 0, 1, ""); }},
        {"SETTINGS with a ragged length", h2_error::frame_size_error,
         [](h2_client& c) { c.frame(std::uint8_t(h2_frame::settings), 0, 0, std::string(5, '\0')); }},
        {"SETTINGS ACK with a payload", h2_error::frame_size_error,
         [](h2_client& c) { c.frame(std::uint8_t(h2_frame::settings), h2_flag_ack, 0,
                                    std::string(6, '\0')); }},
        {"SETTINGS_ENABLE_PUSH neither 0 nor 1", h2_error::protocol_error,
         [](h2_client& c) {
             std::string p("\x00\x02\x00\x00\x00\x02", 6);
             c.frame(std::uint8_t(h2_frame::settings), 0, 0, p);
         }},
        {"SETTINGS_MAX_FRAME_SIZE below 2^14", h2_error::protocol_error,
         [](h2_client& c) {
             std::string p("\x00\x05\x00\x00\x00\x01", 6);
             c.frame(std::uint8_t(h2_frame::settings), 0, 0, p);
         }},
        {"SETTINGS_INITIAL_WINDOW_SIZE above 2^31-1", h2_error::flow_control_error,
         [](h2_client& c) {
             std::string p("\x00\x04\xff\xff\xff\xff", 6);
             c.frame(std::uint8_t(h2_frame::settings), 0, 0, p);
         }},
        {"GOAWAY on a non-zero stream", h2_error::protocol_error,
         [](h2_client& c) { c.frame(std::uint8_t(h2_frame::goaway), 0, 1, std::string(8, '\0')); }},
        {"RST_STREAM with the wrong length", h2_error::frame_size_error,
         [](h2_client& c) { c.frame(std::uint8_t(h2_frame::rst_stream), 0, 1, std::string(3, '\0')); }},
        {"RST_STREAM on an idle stream", h2_error::protocol_error,
         [](h2_client& c) { c.rst(7, 0); }},
        {"WINDOW_UPDATE with the wrong length", h2_error::frame_size_error,
         [](h2_client& c) { c.frame(std::uint8_t(h2_frame::window_update), 0, 0, "abc"); }},
        {"WINDOW_UPDATE of zero on the connection", h2_error::protocol_error,
         [](h2_client& c) { c.window_update(0, 0); }},
        {"WINDOW_UPDATE on an idle stream", h2_error::protocol_error,
         [](h2_client& c) { c.window_update(9, 1); }},
        {"CONTINUATION without HEADERS", h2_error::protocol_error,
         [](h2_client& c) { c.frame(std::uint8_t(h2_frame::continuation), h2_flag_end_headers,
                                    1, ""); }},
        {"a frame between HEADERS and CONTINUATION", h2_error::protocol_error,
         [](h2_client& c) {
             c.frame(std::uint8_t(h2_frame::headers), 0, 1,
                     raw_literal_block(h2_client::get("/hello")));
             c.frame(std::uint8_t(h2_frame::ping), 0, 0, std::string(8, '\0'));
         }},
        {"a frame larger than SETTINGS_MAX_FRAME_SIZE", h2_error::frame_size_error,
         [](h2_client& c) {
             c.frame(std::uint8_t(h2_frame::data), 0, 1, std::string(16385, 'x'));
         }},
        {"DATA whose padding exceeds the frame", h2_error::protocol_error,
         [](h2_client& c) {
             c.request(1, h2_client::get("/hello"), /*end_stream*/ false);
             std::string p;
             p.push_back(char(200));
             p += "short";
             c.frame(std::uint8_t(h2_frame::data), h2_flag_padded, 1, p);
         }},
    };

    h2_server s;
    add_common_routes(s.srv);
    s.start();

    for (const probe& t : probes) {
        h2_client c(s.port);
        c.preface();
        t.send(c);
        std::uint32_t code = 0;
        const bool got = c.wait_goaway(code);
        assert(got);
        assert(code == std::uint32_t(t.want));
    }

    pass("http2: every malformed frame is the connection error RFC 9113 specifies");
}

void test_h2_tolerated_frames()
{
    h2_server s;
    add_common_routes(s.srv);
    s.start();
    h2_client c(s.port);
    c.preface();

    // §5.5: an unknown frame type is ignored, not fatal - this is what lets
    // the protocol be extended without breaking existing servers
    c.frame(0x63, 0xff, 0, std::string(32, 'x'));
    // Undefined flags on a known frame are ignored too
    c.frame(std::uint8_t(h2_frame::ping), 0xfe, 0, std::string(8, 'p'));
    // PRIORITY is parsed and discarded (RFC 9113 deprecates the scheme)
    c.frame(std::uint8_t(h2_frame::priority), 0, 1, std::string("\x00\x00\x00\x00\x10", 5));

    c.request(1, h2_client::get("/hello"));
    h2_client::result r;
    assert(c.collect(1, r));
    assert(r.status == 200 && !r.goaway);

    pass("http2: unknown frame types, undefined flags and PRIORITY are tolerated");
}

void test_h2_ping_and_settings_ack()
{
    h2_server s;
    add_common_routes(s.srv);
    s.start();
    h2_client c(s.port);
    c.preface();

    // Our SETTINGS must be acknowledged
    h2_in f;
    bool acked = false;
    while (c.next(f)) {
        if (f.type == std::uint8_t(h2_frame::settings) && (f.flags & h2_flag_ack)) {
            assert(f.len == 0 && f.sid == 0);
            acked = true;
            break;
        }
    }
    assert(acked);

    // A PING comes back with the ACK flag and the identical payload
    const std::string opaque("\x01\x23\x45\x67\x89\xab\xcd\xef", 8);
    c.frame(std::uint8_t(h2_frame::ping), 0, 0, opaque);
    assert(c.wait_type(std::uint8_t(h2_frame::ping), f));
    assert(f.flags & h2_flag_ack);
    assert(f.payload == opaque);
    assert(f.sid == 0);

    // A PING that is already an ACK gets no answer, so the next thing on the
    // wire is the response to a real request
    c.frame(std::uint8_t(h2_frame::ping), h2_flag_ack, 0, opaque);
    c.request(1, h2_client::get("/hello"));
    h2_client::result r;
    assert(c.collect(1, r));
    assert(r.status == 200);

    pass("http2: SETTINGS are acknowledged and PING is echoed exactly once");
}

void test_h2_stream_states()
{
    h2_server s;
    add_common_routes(s.srv);
    s.start();

    {   // §5.1: DATA on a half-closed (remote) stream is STREAM_CLOSED
        h2_client c(s.port);
        c.preface();
        c.request(1, h2_client::get("/hello"));
        h2_client::result r;
        assert(c.collect(1, r));
        assert(r.status == 200);
        c.frame(std::uint8_t(h2_frame::data), 0, 1, "late");
        std::uint32_t code = 0;
        h2_in f;
        bool saw = false;
        while (c.next(f)) {
            if (f.type == std::uint8_t(h2_frame::rst_stream) && f.sid == 1) {
                code = h2_client::be32(f.payload.data());
                saw = true;
                break;
            }
            if (f.type == std::uint8_t(h2_frame::goaway))
                break;
        }
        assert(saw && code == std::uint32_t(h2_error::stream_closed));
    }
    {   // §5.1.1: identifiers only go up. Stream 3, then stream 1, is fatal.
        h2_client c(s.port);
        c.preface();
        c.request(3, h2_client::get("/hello"));
        h2_client::result r;
        assert(c.collect(3, r));
        c.request(1, h2_client::get("/hello"));
        expect_connection_error(c, h2_error::protocol_error);
    }
    {   // A second HEADERS on an open stream, without END_STREAM, is not
        // trailers and is not a second request
        h2_client c(s.port);
        c.preface();
        c.request(1, h2_client::get("/echo"), /*end_stream*/ false);
        c.request(1, h2_client::get("/echo"), /*end_stream*/ false);
        h2_client::result r;
        assert(c.collect(1, r));
        assert(r.reset && r.reset_code == std::uint32_t(h2_error::protocol_error));
    }

    pass("http2: the stream state machine refuses frames the RFC forbids");
}

void test_h2_malformed_requests()
{
    struct probe {
        const char* what;
        std::vector<hpack::field> fields;
    };
    const std::vector<probe> probes = {
        {"uppercase field name",
         {{":method", "GET"}, {":scheme", "http"}, {":path", "/hello"},
          {":authority", "t"}, {"X-Bad", "1"}}},
        {"connection-specific field",
         {{":method", "GET"}, {":scheme", "http"}, {":path", "/hello"},
          {":authority", "t"}, {"connection", "keep-alive"}}},
        {"transfer-encoding",
         {{":method", "GET"}, {":scheme", "http"}, {":path", "/hello"},
          {":authority", "t"}, {"transfer-encoding", "chunked"}}},
        {"te other than trailers",
         {{":method", "GET"}, {":scheme", "http"}, {":path", "/hello"},
          {":authority", "t"}, {"te", "gzip"}}},
        {"missing :path",
         {{":method", "GET"}, {":scheme", "http"}, {":authority", "t"}}},
        {"empty :path",
         {{":method", "GET"}, {":scheme", "http"}, {":path", ""}, {":authority", "t"}}},
        {"missing :method",
         {{":scheme", "http"}, {":path", "/hello"}, {":authority", "t"}}},
        {"missing :scheme",
         {{":method", "GET"}, {":path", "/hello"}, {":authority", "t"}}},
        {"duplicate pseudo-header",
         {{":method", "GET"}, {":method", "POST"}, {":scheme", "http"},
          {":path", "/hello"}, {":authority", "t"}}},
        {"unknown pseudo-header",
         {{":method", "GET"}, {":scheme", "http"}, {":path", "/hello"},
          {":authority", "t"}, {":protocol", "websocket"}}},
        {"response pseudo-header in a request",
         {{":method", "GET"}, {":scheme", "http"}, {":path", "/hello"},
          {":status", "200"}}},
        {"pseudo-header after a regular one",
         {{":method", "GET"}, {"x-ok", "1"}, {":scheme", "http"},
          {":path", "/hello"}, {":authority", "t"}}},
        {"field value with a leading space",
         {{":method", "GET"}, {":scheme", "http"}, {":path", "/hello"},
          {":authority", "t"}, {"x-ok", " lead"}}},
        {"field value containing a newline",
         {{":method", "GET"}, {":scheme", "http"}, {":path", "/hello"},
          {":authority", "t"}, {"x-ok", "a\nb"}}},
        {"path that is not absolute",
         {{":method", "GET"}, {":scheme", "http"}, {":path", "hello"},
          {":authority", "t"}}},
    };

    h2_server s;
    add_common_routes(s.srv);
    s.start();

    // All on one connection: a malformed request is a *stream* error, so the
    // connection has to survive every one of them
    h2_client c(s.port);
    c.preface();
    std::uint32_t sid = 1;
    for (const probe& t : probes) {
        c.request(sid, t.fields);
        h2_client::result r;
        const bool got = c.collect(sid, r);
        assert(got);
        assert(!r.goaway);
        assert(r.reset && r.reset_code == std::uint32_t(h2_error::protocol_error));
        sid += 2;
    }
    // ... and still serves a good request afterwards
    c.request(sid, h2_client::get("/hello"));
    h2_client::result ok;
    assert(c.collect(sid, ok));
    assert(ok.status == 200);

    pass("http2: malformed requests are stream errors and never kill the connection");
}

void test_h2_continuation()
{
    h2_server s;
    add_common_routes(s.srv);
    s.start();
    h2_client c(s.port);
    c.preface();

    // A header block split across HEADERS and two CONTINUATIONs
    const std::string block = raw_literal_block(h2_client::get("/hello", "split.test"));
    const std::size_t a = block.size() / 3, b = block.size() / 3;
    c.frame(std::uint8_t(h2_frame::headers), h2_flag_end_stream, 1, block.substr(0, a));
    c.frame(std::uint8_t(h2_frame::continuation), 0, 1, block.substr(a, b));
    c.frame(std::uint8_t(h2_frame::continuation), h2_flag_end_headers, 1,
            block.substr(a + b));

    h2_client::result r;
    assert(c.collect(1, r));
    assert(r.status == 200 && r.body == "hello split.test");

    pass("http2: a header block reassembles across CONTINUATION frames");
}

void test_h2_continuation_flood()
{
    // CVE-2024-27316: CONTINUATIONs that never set END_HEADERS cost the
    // attacker nine bytes each and cost the server unbounded memory
    h2_config cfg;
    cfg.max_header_block = 4096;
    h2_server s(cfg);
    add_common_routes(s.srv);
    s.start();

    h2_client c(s.port);
    c.preface();
    c.frame(std::uint8_t(h2_frame::headers), 0, 1,
            raw_literal_block(h2_client::get("/hello")));
    for (int i = 0; i < 64; ++i)
        c.frame(std::uint8_t(h2_frame::continuation), 0, 1, std::string(1024, '\0'));

    expect_connection_error(c, h2_error::enhance_your_calm);
    pass("http2: an endless CONTINUATION run is refused before it costs memory");
}

// ================================================== HTTP/2: flow control

void test_h2_flow_control()
{
    h2_server s;
    add_common_routes(s.srv);
    s.start();

    h2_client c(s.port);
    // Tell the server every stream starts with a 10-octet window
    c.preface({{h2_settings_initial_window_size, 10}});
    c.request(1, h2_client::get("/big"));       // a 100-byte body

    // Headers plus exactly ten octets, then silence: the window is spent
    std::string body;
    bool ended = false;
    h2_in f;
    while (c.next(f)) {
        if (f.sid == 1 && f.type == std::uint8_t(h2_frame::data)) {
            body += f.payload;
            if (f.flags & h2_flag_end_stream)
                ended = true;
            break;
        }
        assert(f.type != std::uint8_t(h2_frame::goaway));
    }
    assert(body.size() == 10);
    assert(!ended);

    // Open the window in two steps and watch the rest arrive
    c.window_update(1, 40);
    while (body.size() < 50 && c.next(f))
        if (f.sid == 1 && f.type == std::uint8_t(h2_frame::data))
            body += f.payload;
    assert(body.size() == 50);

    c.window_update(1, 1000);
    while (!ended && c.next(f)) {
        if (f.sid == 1 && f.type == std::uint8_t(h2_frame::data)) {
            body += f.payload;
            if (f.flags & h2_flag_end_stream)
                ended = true;
        }
    }
    assert(ended);
    assert(body == std::string(100, 'z'));

    pass("http2: outbound DATA respects the per-stream window and resumes on WINDOW_UPDATE");
}

void test_h2_initial_window_is_a_delta()
{
    // §6.9.2: changing SETTINGS_INITIAL_WINDOW_SIZE adjusts the windows of
    // streams that already exist by the difference. A server that treats it
    // as an assignment loses track of what it has already sent.
    h2_server s;
    add_common_routes(s.srv);
    s.start();

    h2_client c(s.port);
    c.preface({{h2_settings_initial_window_size, 10}});
    c.request(1, h2_client::get("/big"));

    std::string body;
    h2_in f;
    while (c.next(f)) {
        if (f.sid == 1 && f.type == std::uint8_t(h2_frame::data)) {
            body += f.payload;
            break;
        }
    }
    assert(body.size() == 10);

    // Raise the initial size to 60: the delta is +50, and stream 1 has
    // already spent 10, so it may now send 50 more - not 60.
    std::string p("\x00\x04\x00\x00\x00\x3c", 6);
    c.frame(std::uint8_t(h2_frame::settings), 0, 0, p);
    while (body.size() < 60 && c.next(f))
        if (f.sid == 1 && f.type == std::uint8_t(h2_frame::data))
            body += f.payload;
    assert(body.size() == 60);

    pass("http2: SETTINGS_INITIAL_WINDOW_SIZE is applied as a delta to open streams");
}

void test_h2_inbound_window_replenishes()
{
    // A body larger than the default 65535-octet stream window only completes
    // if the server issues WINDOW_UPDATEs as it consumes
    server_config sc;
    sc.limits.max_body = 4u * 1024 * 1024;
    h2_server s(h2_config{}, sc);
    add_common_routes(s.srv);
    s.start();

    h2_client c(s.port);
    c.auto_window(true);                        // the echo comes back the same size
    c.preface();
    const std::size_t total = 200u * 1024;
    std::vector<hpack::field> f = {{":method", "POST"}, {":scheme", "http"},
                                   {":path", "/echo"}, {":authority", "t"},
                                   {"content-length", std::to_string(total)}};
    c.frame(std::uint8_t(h2_frame::headers), h2_flag_end_headers, 1, raw_literal_block(f));
    assert(c.send_body(1, std::string(total, 'q'), /*end_stream*/ true));

    h2_client::result r;
    assert(c.collect(1, r));
    assert(r.status == 200);
    assert(r.body.size() == total);
    assert(r.body.find_first_not_of('q') == std::string::npos);

    pass("http2: inbound flow control replenishes so a body beyond one window completes");
}

// ==================================================== HTTP/2: abuse limits

void test_h2_rapid_reset()
{
    // The 2023 Rapid Reset pattern: HEADERS then RST_STREAM, forever. Nine
    // bytes of attacker effort buys a whole request of server work, so the
    // only defence that scales is a rate limit on the resets themselves.
    h2_config cfg;
    cfg.rst_burst = 5;
    cfg.rst_per_second = 0.0;
    h2_server s(cfg);
    add_common_routes(s.srv);
    s.start();

    h2_client c(s.port);
    c.preface();
    for (std::uint32_t sid = 1; sid < 41; sid += 2) {
        c.request(sid, h2_client::get("/hello"), /*end_stream*/ false);
        c.rst(sid, std::uint32_t(h2_error::cancel));
    }
    expect_connection_error(c, h2_error::enhance_your_calm);

    pass("http2: a RST_STREAM flood is refused (the Rapid Reset class)");
}

void test_h2_settings_and_ping_flood()
{
    h2_config cfg;
    cfg.control_burst = 5;
    cfg.control_per_second = 0.0;
    h2_server s(cfg);
    add_common_routes(s.srv);
    s.start();

    h2_client c(s.port);
    c.preface();                                // one SETTINGS spent already
    for (int i = 0; i < 40; ++i)
        c.frame(std::uint8_t(h2_frame::ping), 0, 0, std::string(8, char(i)));
    expect_connection_error(c, h2_error::enhance_your_calm);

    pass("http2: a PING/SETTINGS flood is refused before the ACKs amplify it");
}

void test_h2_concurrent_stream_limit()
{
    h2_config cfg;
    cfg.max_concurrent_streams = 2;
    h2_server s(cfg);
    add_common_routes(s.srv);
    s.start();

    h2_client c(s.port);
    c.preface();
    // Two streams held open by an unfinished body, then a third
    c.request(1, h2_client::get("/echo"), /*end_stream*/ false);
    c.request(3, h2_client::get("/echo"), /*end_stream*/ false);
    c.request(5, h2_client::get("/echo"), /*end_stream*/ false);

    h2_client::result r;
    assert(c.collect(5, r));
    assert(r.reset && r.reset_code == std::uint32_t(h2_error::refused_stream));
    assert(!r.goaway);                          // and the connection lives

    pass("http2: opening more than SETTINGS_MAX_CONCURRENT_STREAMS is refused per stream");
}

void test_h2_header_list_cap()
{
    h2_config cfg;
    cfg.max_header_list_size = 512;
    h2_server s(cfg);
    add_common_routes(s.srv);
    s.start();

    h2_client c(s.port);
    c.preface();
    std::vector<hpack::field> f = h2_client::get("/hello");
    f.push_back({"x-large", std::string(2048, 'a')});
    c.request(1, f);

    h2_client::result r;
    assert(c.collect(1, r));
    assert(r.reset && r.reset_code == std::uint32_t(h2_error::enhance_your_calm));

    // The connection survives, and so does the HPACK state
    c.request(3, h2_client::get("/hello"));
    h2_client::result ok;
    assert(c.collect(3, ok));
    assert(ok.status == 200);

    pass("http2: an oversized header list is a stream error, not a lost connection");
}

// ================================================== protocol selection

void test_alpn_selector()
{
    // One server, one port, two protocols - chosen per connection with no
    // rebuild. This is the claim the whole delegate architecture rests on.
    h2_server s;
    add_common_routes(s.srv);
    s.start();

    {   // An h2c client, recognised by its preface
        h2_client c(s.port);
        c.preface();
        c.request(1, h2_client::get("/hello", "h2.test"));
        h2_client::result r;
        assert(c.collect(1, r));
        assert(r.status == 200 && r.body == "hello h2.test");
    }
    {   // An ordinary HTTP/1.1 client on the same listener
        h2_client c(s.port);
        c.write("GET /hello HTTP/1.1\r\nHost: one.test\r\nConnection: close\r\n\r\n");
        const std::string all = c.read_all();
        assert(all.compare(0, 15, "HTTP/1.1 200 OK") == 0);
        assert(all.find("hello one.test") != std::string::npos);
    }
    {   // A request that merely starts with 'P' is not an h2 preface
        h2_client c(s.port);
        c.write("POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 3\r\n"
                "Connection: close\r\n\r\nabc");
        const std::string all = c.read_all();
        assert(all.compare(0, 15, "HTTP/1.1 200 OK") == 0);
        assert(all.size() >= 3 && all.compare(all.size() - 3, 3, "abc") == 0);
    }

    pass("http2: one listener serves h2c and HTTP/1.1, chosen per connection");
}

// A transport that is plaintext in every respect except that it reports an
// ALPN result. That is all a TLS delegate contributes to protocol selection,
// so this exercises the real ALPN branch with no TLS, no certificates and no
// third-party dependency - which is the point of the transport being a
// run-time delegate rather than a compile-time choice.
class alpn_announcing_transport final : public transport_delegate {
public:
    explicit alpn_announcing_transport(const char* what) : what_(what) {}
    const char* name() const noexcept override { return "plain+alpn"; }
    const char* alpn() const noexcept override { return what_; }

    bool wire_in(const char* data, std::size_t n, std::string& app_in, std::string&) override
    {
        app_in.append(data, n);
        return true;
    }
    bool app_out(const char* data, std::size_t n, std::string& wire_out) override
    {
        wire_out.append(data, n);
        return true;
    }

private:
    const char* what_;
};

void test_alpn_chooses_h2()
{
    {   // ALPN said h2: the connection is HTTP/2 before a single byte arrives
        h2_server s;
        add_common_routes(s.srv);
        s.srv.transport_factory([] {
            return std::unique_ptr<transport_delegate>(new alpn_announcing_transport("h2"));
        });
        s.start();

        h2_client c(s.port);
        c.preface();
        c.request(1, h2_client::get("/hello", "alpn.test"));
        h2_client::result r;
        assert(c.collect(1, r));
        assert(r.status == 200 && r.body == "hello alpn.test");

        // And because ALPN decided, a non-h2 client on that listener is a
        // protocol error rather than something to sniff at
        h2_client c2(s.port);
        c2.write("GET /hello HTTP/1.1\r\nHost: t\r\n\r\n");
        expect_connection_error(c2, h2_error::protocol_error);
    }
    {   // ALPN said http/1.1: h2 is not offered, even to a client that asks
        h2_server s;
        add_common_routes(s.srv);
        s.srv.transport_factory([] {
            return std::unique_ptr<transport_delegate>(
                new alpn_announcing_transport("http/1.1"));
        });
        s.start();

        h2_client c(s.port);
        c.write("GET /hello HTTP/1.1\r\nHost: one.test\r\nConnection: close\r\n\r\n");
        const std::string all = c.read_all();
        assert(all.compare(0, 15, "HTTP/1.1 200 OK") == 0);
        assert(all.find("hello one.test") != std::string::npos);

        // An h2 preface is not sniffed for once ALPN has spoken: HTTP/1.1
        // answers it as the unsupported-version request it is
        h2_client c2(s.port);
        c2.write(kH2Preface, kH2PrefaceLen);
        const std::string reply = c2.read_all();
        assert(reply.compare(0, 9, "HTTP/1.1 ") == 0);
        assert(reply.find(" 505 ") != std::string::npos);
    }

    pass("http2: ALPN picks the protocol delegate before any bytes are parsed");
}

void test_h2_only_mode()
{
    // The other deployment: behind a proxy that has already done the ALPN,
    // there is nothing to sniff and nothing to fall back to
    h2_server s(h2_config{}, server_config{}, /*only_h2*/ true);
    add_common_routes(s.srv);
    s.start();

    {   // h2c still works, without a selector in the way
        h2_client c(s.port);
        c.preface();
        c.request(1, h2_client::get("/hello", "only.test"));
        h2_client::result r;
        assert(c.collect(1, r));
        assert(r.status == 200 && r.body == "hello only.test");
    }
    {   // And an HTTP/1.1 request is a connection error, not a 400 page.
        // This is the case h2spec's §3.5 asks about, and it is the one place
        // the two modes are allowed to disagree.
        h2_client c(s.port);
        c.write("GET /hello HTTP/1.1\r\nHost: t\r\n\r\n");
        expect_connection_error(c, h2_error::protocol_error);
    }

    pass("http2: h2-only mode answers a non-h2 client with GOAWAY, not HTTP/1.1");
}

void test_h2_goaway_on_shutdown()
{
    // A connection error must reach the client as GOAWAY before the socket
    // closes, or the client is left guessing
    h2_server s;
    add_common_routes(s.srv);
    s.start();

    h2_client c(s.port);
    c.preface();
    c.request(1, h2_client::get("/hello"));
    h2_client::result r;
    assert(c.collect(1, r));
    assert(r.status == 200);

    // Stream 1 is finished; a GOAWAY that names it should still be well formed
    c.frame(std::uint8_t(h2_frame::data), 0, 0, "x");     // DATA on stream 0
    h2_in f;
    assert(c.wait_type(std::uint8_t(h2_frame::goaway), f));
    assert(f.sid == 0);
    assert(f.len >= 8);
    assert(h2_client::be32(f.payload.data()) >= 1);       // last stream we processed
    assert(h2_client::be32(f.payload.data() + 4) == std::uint32_t(h2_error::protocol_error));

    pass("http2: a connection error sends a well-formed GOAWAY before closing");
}

} // namespace

void run_http2_tests()
{
    test_hpack_integers();
    test_hpack_huffman();
    test_hpack_representations();
    test_hpack_requests();
    test_hpack_responses();
    test_hpack_dynamic_table();
    test_hpack_refusals();
    test_hpack_encoder_round_trip();

    test_h2c_prior_knowledge();
    test_h2_request_body_and_head();
    test_h2_multiplexing();
    test_h2_streamed_response();

    test_h2_bad_preface();
    test_h2_frame_refusals();
    test_h2_tolerated_frames();
    test_h2_ping_and_settings_ack();
    test_h2_stream_states();
    test_h2_malformed_requests();
    test_h2_continuation();
    test_h2_continuation_flood();

    test_h2_flow_control();
    test_h2_initial_window_is_a_delta();
    test_h2_inbound_window_replenishes();

    test_h2_rapid_reset();
    test_h2_settings_and_ping_flood();
    test_h2_concurrent_stream_limit();
    test_h2_header_list_cap();

    test_alpn_selector();
    test_alpn_chooses_h2();
    test_h2_only_mode();
    test_h2_goaway_on_shutdown();
}

#else // !SNICHOLLS_HAS_HTTP2

// Windows: HTTP/2 follows the server, which follows the reactor
void run_http2_tests() {}

#endif
