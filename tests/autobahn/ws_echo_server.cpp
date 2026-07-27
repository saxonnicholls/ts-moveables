//
//  ws_echo_server.cpp
//  TSMoveables
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - the server under test for Autobahn|Testsuite
//
//  Autobahn is the external grader for RFC 6455: several hundred cases that
//  attack framing, fragmentation, UTF-8, control frames, close codes and
//  limits, run by a fuzzing client against an echo server. This is that echo
//  server, and it is deliberately the thinnest thing that can be: every
//  message goes straight back with the same opcode, so what Autobahn measures
//  is websocket.hpp rather than anything written here.
//
//  Run it via scripts/run_autobahn.sh, which starts this, drives the fuzzing
//  client at it and summarises the report.
//
//      ./build/ws_echo_server [port]         (default 9001, the Autobahn one)
//

#include "../../TSMoveables/http/websocket.hpp"
#ifdef SNICHOLLS_AUTOBAHN_DEFLATE
#include "../../TSMoveables/http/websocket_deflate.hpp"
#endif

#include <cstdio>
#include <cstdlib>

#if !SNICHOLLS_HAS_WEBSOCKET

int main()
{
    std::printf("autobahn echo server: POSIX only - skipped\n");
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
        static_cast<std::uint16_t>(argc > 1 ? std::atoi(argv[1]) : 9001);

    server_config cfg;
    // Autobahn's 9.x cases push single messages up to 16 MiB, and some cases
    // deliberately sit idle - neither should be mistaken for abuse here
    cfg.idle_timeout = std::chrono::seconds{300};
    cfg.request_timeout = std::chrono::seconds{120};
    cfg.write_high_water = 32u * 1024 * 1024;

    ws_config ws;
    ws.max_message = 20u * 1024 * 1024;
    ws.max_frame = 20u * 1024 * 1024;
#ifdef SNICHOLLS_AUTOBAHN_DEFLATE
    // Groups 12 and 13 exercise permessage-deflate; without the extension
    // wired in they report UNIMPLEMENTED, which is honest but not a pass
    permessage_deflate::options dopt;
    dopt.max_decompressed = 24u * 1024 * 1024;
    ws.extension_factory = deflate_factory(dopt);
#endif

    server srv(cfg);
    g_srv = &srv;
    std::signal(SIGINT, stop_it);
    std::signal(SIGTERM, stop_it);

    // Every path is the echo endpoint: Autobahn addresses cases as /runCase?...
    srv.get("/*", websocket_route([](websocket sock) {
        // The socket arrives as an argument rather than a capture, so the slot
        // holds no strong reference to the connection it lives on
        sock.on_message([](websocket s, const ws_message& m) {
            if (m.is_text)
                s.send_text(m.data);            // text back as text
            else
                s.send_binary(m.data);          // binary back as binary
        });
    }, ws));

    // The fuzzing client may run in a container, which cannot reach loopback
    const char* host = std::getenv("BIND_HOST");
    const std::string bind_host = host && *host ? host : "127.0.0.1";
    const std::uint16_t bound = srv.listen(bind_host, port);
    std::printf("autobahn echo server listening on ws://%s:%u\n",
                bind_host.c_str(), unsigned(bound));
    std::fflush(stdout);
    srv.run();
    return 0;
}

#endif
