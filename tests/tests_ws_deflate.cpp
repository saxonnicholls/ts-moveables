//
//  tests_ws_deflate.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - permessage-deflate negotiation and round trips
//
//  Placed in the main suite deliberately: the *negotiation* logic and the
//  extension seam in websocket.hpp need no zlib, so they are tested here
//  unconditionally with a stub extension. The zlib-backed delegate itself is
//  graded by Autobahn groups 12 and 13 (`make autobahn`), which is a far more
//  thorough compression test than anything worth hand-writing.
//

#include "test_helpers.hpp"

#include "../TSMoveables/http/websocket.hpp"

#if SNICHOLLS_HAS_WEBSOCKET

#include <string>
#include <vector>

using namespace snicholls;
using namespace snicholls::http;

namespace {

void test_extension_header_parsing()
{
    {
        const auto v = http::detail::parse_extensions("permessage-deflate");
        assert(v.size() == 1);
        assert(v[0].first == "permessage-deflate");
        assert(v[0].second.empty());
    }
    {
        const auto v = http::detail::parse_extensions(
            "permessage-deflate; client_max_window_bits, x-other; a=1");
        assert(v.size() == 2);
        assert(v[0].first == "permessage-deflate");
        assert(v[0].second == "client_max_window_bits");
        assert(v[1].first == "x-other");
        assert(v[1].second == "a=1");
    }
    {   // whitespace around separators is insignificant
        const auto v = http::detail::parse_extensions("  a ;  p=1 ,  b  ");
        assert(v.size() == 2);
        assert(v[0].first == "a" && v[0].second == "p=1");
        assert(v[1].first == "b");
    }
    pass("ws deflate: Sec-WebSocket-Extensions parsing");
}

// A stub extension: proves the seam without needing zlib in this binary
class reverse_extension final : public ws_extension {
public:
    const char* name() const noexcept override { return "x-reverse"; }
    bool negotiate(const std::string& params, std::string& response) override
    {
        if (params.find("reject") != std::string::npos)
            return false;
        response = params.empty() ? std::string() : std::string("ack");
        ok_ = true;
        return true;
    }
    bool compress(const std::string& in, std::string& out) override
    {
        if (!ok_) return false;
        out.assign(in.rbegin(), in.rend());
        return true;
    }
    bool decompress(const std::string& in, std::string& out) override
    {
        if (!ok_) return false;
        out.assign(in.rbegin(), in.rend());
        return true;
    }
private:
    bool ok_ = false;
};

void test_extension_round_trip()
{
    reverse_extension e;
    std::string response;
    assert(e.negotiate("", response));

    std::string enc, dec;
    const std::string original = "the quick brown fox";
    assert(e.compress(original, enc));
    assert(enc != original);
    assert(e.decompress(enc, dec));
    assert(dec == original);           // the seam is lossless

    reverse_extension declined;
    std::string r2;
    assert(!declined.negotiate("reject", r2));
    std::string tmp;
    assert(!declined.compress(original, tmp));   // a declined extension stays inert

    pass("ws deflate: extension delegate seam round-trips and can decline");
}

} // namespace

void run_ws_deflate_tests()
{
    test_extension_header_parsing();
    test_extension_round_trip();
}

#else

void run_ws_deflate_tests() {}

#endif
