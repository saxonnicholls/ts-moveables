//
//  h2_server.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - the server under test for h2spec
//
//  h2spec is the external grader for RFC 9113 and RFC 7541: about 150 cases
//  that attack framing, the stream state machine, flow control, HPACK and the
//  HTTP semantics layer, driven as a client against a real server. This is
//  that server, and it is deliberately the thinnest thing that can be - one
//  route that answers everything with 200 - so what h2spec measures is
//  http2.hpp rather than anything written here.
//
//  Run it via scripts/run_h2spec.sh, which starts this, drives h2spec at it
//  and summarises the report.
//
//      ./build/h2_server [port]              (default 8080)
//

#include "../../TSMoveables/http/http2.hpp"

#include <cstdio>
#include <cstdlib>

#if !SNICHOLLS_HAS_HTTP2

int main()
{
    std::printf("h2spec server: POSIX only - skipped\n");
    return 0;
}

#else

#include <csignal>
#include <string>

using namespace snicholls;
using namespace snicholls::http;

namespace {
server* g_srv = nullptr;
void stop_it(int) { if (g_srv) g_srv->stop(); }
} // namespace

int main(int argc, char** argv)
{
    const std::uint16_t port =
        static_cast<std::uint16_t>(argc > 1 ? std::atoi(argv[1]) : 8080);

    server_config cfg;
    // h2spec opens a fresh connection per case and some cases deliberately go
    // quiet waiting to see what we do; neither is abuse
    cfg.idle_timeout = std::chrono::seconds{120};
    cfg.request_timeout = std::chrono::seconds{60};
    cfg.limits.max_body = 16u * 1024 * 1024;

    h2_config h2;
    // The abuse limits stay on - a grader run with the defences disabled
    // grades a server nobody should deploy - but the thresholds are lifted
    // clear of h2spec's own traffic, which opens and resets streams by design
    // in the 5.1 and 6.4 groups.
    h2.rst_burst = 10000.0;
    h2.rst_per_second = 10000.0;
    h2.control_burst = 10000.0;
    h2.control_per_second = 10000.0;
    h2.max_header_list_size = 256u * 1024;

    server srv(cfg);
    g_srv = &srv;
    std::signal(SIGINT, stop_it);
    std::signal(SIGTERM, stop_it);
    // h2-only by default, because that is what h2spec's cleartext run models:
    // every connection is HTTP/2 from its first octet, with no HTTP/1.1 to
    // fall back to. Set H2_MIXED=1 to grade the protocol-selecting listener
    // instead; it scores 146/147, and the one difference is §3.5's "invalid
    // connection preface", which a mixed listener answers as the malformed
    // HTTP/1.1 request it also is. That difference is a property of serving
    // two protocols on one port, not a defect - see enable_http2_only().
    if (std::getenv("H2_MIXED"))
        enable_http2(srv, h2);
    else
        enable_http2_only(srv, h2);

    // h2spec addresses every case at "/", but answer everything anyway: a 404
    // would still be a valid response and would still grade, but it makes the
    // failure reports harder to read
    auto ok = [](const request& req, responder r) {
        r.send(200, "text/plain", "ts-moveables h2: " + req.path + "\n");
    };
    srv.get("/*", ok);
    srv.post("/*", ok);
    srv.put("/*", ok);
    srv.del("/*", ok);
    srv.head("/*", ok);
    srv.options("/*", ok);
    srv.not_found(ok);

    // h2spec may run in a container, which cannot reach loopback
    const char* host = std::getenv("BIND_HOST");
    const std::string bind_host = host && *host ? host : "127.0.0.1";
    const std::uint16_t bound = srv.listen(bind_host, port);
    std::printf("h2spec server listening on http://%s:%u\n",
                bind_host.c_str(), unsigned(bound));
    std::fflush(stdout);
    srv.run();
    return 0;
}

#endif
