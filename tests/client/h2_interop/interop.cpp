//
//  interop.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - http::client's HTTP/2 against servers that are not ours: Go's standard library (h2c and
//  h2 over TLS by ALPN) and, when given, nghttp2's nghttpd. Run by `make h2-client-interop`, which starts them.
//
//      interop --go-h2c PORT --go-tls PORT --ca FILE [--nghttpd PORT --nghttpd-file NAME --nghttpd-size BYTES]
//

#include "../../test_helpers.hpp"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "http/client.hpp"
#include "tls/openssl.hpp"

namespace {

using namespace std::chrono_literals;
using snicholls::event_loop;
namespace http = snicholls::http;

struct options {
    std::string go_h2c, go_tls, ca, nghttpd, nghttpd_file;
    std::size_t nghttpd_size = 0;
};

std::vector<http::client_result> run(http::client_config cfg, std::vector<http::client_request> reqs,
                                     http::client_stats* stats = nullptr)
{
    event_loop loop;
    std::vector<std::optional<http::client_result>> out(reqs.size());
    std::size_t done = 0;
    {
        http::client c(loop, std::move(cfg));
        for (std::size_t i = 0; i < reqs.size(); ++i)
            c.request(reqs[i], [&out, &done, i](http::client_result r) {
                out[i].emplace(std::move(r));
                ++done;
            });
        const auto until = std::chrono::steady_clock::now() + 20s;
        while (done < reqs.size() && std::chrono::steady_clock::now() < until) loop.run_once(10ms);
        if (stats) *stats = c.stats();
    }
    assert(done == reqs.size());
    std::vector<http::client_result> r;
    for (auto& o : out) r.push_back(std::move(*o));
    return r;
}

http::client_config h2c()
{
    http::client_config cfg;
    cfg.http2_prior_knowledge = true;
    return cfg;
}

void check_ok(const http::client_result& r, int status)
{
    if (r.is_err()) {
        std::cerr << "request failed: " << r.unwrap_err_unchecked().what() << "\n";
        assert(false);
    }
    assert(r->status == status && r->http_major == 2);
}

void against_go(const options& o)
{
    const std::string base = "http://127.0.0.1:" + o.go_h2c;
    const std::string up(2u << 20, 'u');
    http::client_stats st;
    auto r = run(h2c(),
                 {{"GET", base + "/hello", {}, {}, {}},
                  {"POST", base + "/echo", {{"Content-Type", "application/octet-stream"}}, up, {}},
                  {"GET", base + "/big?n=5242880", {}, {}, {}},
                  {"HEAD", base + "/hello", {}, {}, {}},
                  {"GET", base + "/nocontent", {}, {}, {}}},
                 &st);
    check_ok(r[0], 200);
    assert(r[0]->body == "hello HTTP/2.0 ts-moveables");
    check_ok(r[1], 200);
    assert(r[1]->body == up);
    check_ok(r[2], 200);
    assert(r[2]->body.size() == 5242880u);
    check_ok(r[3], 200);
    assert(r[3]->body.empty());
    check_ok(r[4], 204);
    assert(st.connections_opened == 1 && st.http2_connections == 1);
    pass("go h2c: GET, 2 MiB echoed, 5 MiB down, HEAD and 204 on one connection");

    auto t = run(h2c(), {{"POST", base + "/trailers", {{"Content-Type", "application/grpc"}, {"TE", "trailers"}},
                          std::string("\0\0\0\0\0", 5), {}}});
    check_ok(t[0], 200);
    assert(t[0]->body == std::string("\0\0\0\0\x02ok", 7));
    assert(t[0]->trailers.find("grpc-status") && *t[0]->trailers.find("grpc-status") == "0");
    assert(*t[0]->trailers.find("grpc-message") == "all good");
    pass("go h2c: trailers after the body (gRPC's shape) land in response::trailers");

    std::vector<http::client_request> many;
    for (int i = 0; i < 50; ++i)
        many.push_back({"GET", base + "/delay?ms=" + std::to_string((i * 37) % 120) + "&id=" + std::to_string(i), {},
                        {}, {}});
    auto m = run(h2c(), many, &st);
    for (int i = 0; i < 50; ++i) {
        check_ok(m[std::size_t(i)], 200);
        assert(m[std::size_t(i)]->body == std::to_string(i));
    }
    assert(st.connections_opened == 1);
    pass("go h2c: 50 concurrent requests multiplexed on one connection, each answer matched to its request");

    auto a = run(h2c(), {{"GET", base + "/abort", {}, {}, {}}, {"GET", base + "/hello", {}, {}, {}}}, &st);
    assert(a[0].is_err() && a[0].unwrap_err_unchecked().code() == http::client_errc::stream_reset);
    check_ok(a[1], 200);
    assert(st.connections_opened == 1);
    pass("go h2c: a handler that aborts resets its stream only; the request beside it is answered");

    // TLS: ALPN chooses h2
    http::openssl_client_config ccfg;
    ccfg.ca_file = o.ca;
    ccfg.alpn = {"h2", "http/1.1"};
    auto tls = std::make_shared<http::openssl_client_context>(ccfg);
    http::client_config secure;
    secure.transport_factory = [tls](const std::string& host) {
        return std::unique_ptr<http::transport_delegate>(tls->connect(host).release());
    };
    const std::string sbase = "https://localhost:" + o.go_tls;
    auto s = run(secure,
                 {{"GET", sbase + "/hello", {}, {}, {}},
                  {"POST", sbase + "/trailers", {{"TE", "trailers"}}, "x", {}},
                  {"POST", sbase + "/echo", {}, up, {}}},
                 &st);
    check_ok(s[0], 200);
    assert(s[0]->body == "hello HTTP/2.0 ts-moveables");
    check_ok(s[1], 200);
    assert(*s[1]->trailers.find("grpc-status") == "0");
    check_ok(s[2], 200);
    assert(s[2]->body == up);
    assert(st.connections_opened == 1 && st.http2_connections == 1);
    pass("go https: ALPN agrees h2; trailers and a 2 MiB echo over one verified TLS session");
}

void against_nghttpd(const options& o)
{
    const std::string base = "http://127.0.0.1:" + o.nghttpd;
    http::client_stats st;
    std::vector<http::client_request> reqs;
    for (int i = 0; i < 8; ++i) reqs.push_back({"GET", base + "/" + o.nghttpd_file, {}, {}, {}});
    reqs.push_back({"GET", base + "/no-such-file", {}, {}, {}});
    auto r = run(h2c(), reqs, &st);
    for (int i = 0; i < 8; ++i) {
        check_ok(r[std::size_t(i)], 200);
        assert(r[std::size_t(i)]->body.size() == o.nghttpd_size);
    }
    check_ok(r[8], 404);
    assert(st.connections_opened == 1);
    pass("nghttpd h2c: eight concurrent downloads and a 404 on one connection");
}

} // namespace

int main(int argc, char** argv)
{
    options o;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i], v = argv[i + 1];
        if (k == "--go-h2c") o.go_h2c = v;
        else if (k == "--go-tls") o.go_tls = v;
        else if (k == "--ca") o.ca = v;
        else if (k == "--nghttpd") o.nghttpd = v;
        else if (k == "--nghttpd-file") o.nghttpd_file = v;
        else if (k == "--nghttpd-size") o.nghttpd_size = std::size_t(std::strtoull(v.c_str(), nullptr, 10));
    }
    if (!o.go_h2c.empty())
        against_go(o);
    if (!o.nghttpd.empty())
        against_nghttpd(o);
    std::cout << "\nh2 client interop: all " << tests_run << " checks passed\n";
    return tests_run > 0 ? 0 : 1;
}
