//
//  http_request_path.cpp
//  TSMoveables
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - what does one request actually cost?
//
//  The head-to-head against cpp-httplib showed us ~1.5x slower at low
//  concurrency on Linux, and the convenient explanation was architectural: a
//  reactor pays a poll syscall and an extra wakeup hop where a blocking thread
//  is woken directly with its data. That is true, and it is also exactly the
//  sort of explanation that stops people looking - so this measures the part
//  that has nothing to do with syscalls at all.
//
//  No sockets, no loop, no kernel: parse a request, route it, serialise a
//  response, repeat. Whatever this costs is pure CPU on the hot path, and any
//  of it that is avoidable is throughput we are giving away for nothing.
//
//      make bench-request                 # default: a realistic 12-header GET
//      ./build/http_request_path --headers 2 --routes 20
//

#include "../TSMoveables/http_server.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if !SNICHOLLS_HAS_HTTP_SERVER

int main()
{
    std::printf("request path bench: POSIX only - skipped\n");
    return 0;
}

#else

#include <chrono>
#include <string>
#include <vector>

using namespace snicholls;
using namespace snicholls::http;

namespace {

bool markdown = false;

double seconds_since(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// Just enough of a host to serialise against, with no IO behind it
class null_host final : public connection_host {
public:
    void deliver(request&, std::uint64_t) override {}
    void write_app(const char* d, std::size_t n) override { sink.append(d, n); }
    void protocol_failure(int, const char*) override {}
    const server_config& config() const noexcept override { return cfg; }
    const char* http_date() override { return "Mon, 27 Jul 2026 00:00:00 GMT"; }
    bool live() const noexcept override { return true; }
    void switch_protocol(std::unique_ptr<protocol_delegate>) override {}

    server_config cfg;
    std::string sink;
};

void row(const char* what, double ns, double base_ns)
{
    char rate[64];
    std::snprintf(rate, sizeof rate, "%.0f k/s", 1e6 / ns);
    if (base_ns > 0)
        std::printf(markdown ? "| %s | %.0f ns | %s | %.2fx |\n" : "  %-34s %7.0f ns  %12s  %.2fx\n",
                    what, ns, rate, base_ns / ns);
    else
        std::printf(markdown ? "| %s | %.0f ns | %s | - |\n" : "  %-34s %7.0f ns  %12s\n",
                    what, ns, rate);
}

} // namespace

int main(int argc, char** argv)
{
    int extra_headers = 12;
    int n_routes = 8;
    long iterations = 300000;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--markdown") == 0) markdown = true;
        else if (std::strcmp(argv[i], "--headers") == 0 && i + 1 < argc) extra_headers = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--routes") == 0 && i + 1 < argc) n_routes = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) iterations = std::atol(argv[++i]);
    }

    // A request shaped like real traffic rather than like a benchmark
    std::string wire = "GET /api/v1/users/4242/posts?page=3&limit=50 HTTP/1.1\r\n"
                       "Host: api.example.com\r\n";
    for (int i = 0; i < extra_headers; ++i)
        wire += "X-Header-" + std::to_string(i) + ": some-plausible-value-" + std::to_string(i) + "\r\n";
    wire += "\r\n";

    // A route table with the target near the end, as it would be in an app
    router routes;
    for (int i = 0; i < n_routes - 1; ++i)
        routes.add(method::get, "/other/" + std::to_string(i) + "/:id", [](const request&, responder) {});
    routes.add(method::get, "/api/v1/users/:uid/posts", [](const request&, responder) {});

    const std::string body(256, 'b');
    null_host host;

    // ------------------------------------------------------------ parse, cold
    // A brand-new parser every time: the first request on a connection, where
    // nothing has been allocated yet
    double parse_cold_ns = 0;
    {
        const auto t0 = std::chrono::steady_clock::now();
        long ok = 0;
        for (long i = 0; i < iterations; ++i) {
            request_parser p;
            std::size_t used = 0;
            if (p.parse(wire.data(), wire.size(), used) == request_parser::status::have_request)
                ++ok;
        }
        parse_cold_ns = seconds_since(t0) * 1e9 / double(iterations);
        if (ok != iterations) { std::fprintf(stderr, "parse failed\n"); return 1; }
    }

    // ------------------------------------------------------------ parse, warm
    // One parser reset between requests: every request after the first on a
    // keep-alive connection, which is overwhelmingly the common case and the
    // one buffer reuse is for
    double parse_warm_ns = 0;
    {
        request_parser p;
        const auto t0 = std::chrono::steady_clock::now();
        long ok = 0;
        for (long i = 0; i < iterations; ++i) {
            std::size_t used = 0;
            if (p.parse(wire.data(), wire.size(), used) == request_parser::status::have_request)
                ++ok;
            p.reset();
        }
        parse_warm_ns = seconds_since(t0) * 1e9 / double(iterations);
        if (ok != iterations) { std::fprintf(stderr, "warm parse failed\n"); return 1; }
    }

    // ---------------------------------------------------------------- route
    double route_ns = 0;
    {
        request_parser p;
        std::size_t used = 0;
        p.parse(wire.data(), wire.size(), used);
        const request& req = p.message();

        const auto t0 = std::chrono::steady_clock::now();
        long hits = 0;
        for (long i = 0; i < iterations; ++i) {
            std::unordered_map<std::string, std::string> params;
            bool exists = false;
            std::string allowed;
            if (routes.match(req, params, exists, allowed))
                ++hits;
        }
        route_ns = seconds_since(t0) * 1e9 / double(iterations);
        if (hits != iterations) { std::fprintf(stderr, "route failed\n"); return 1; }
    }

    // ------------------------------------------------------------ serialise
    double ser_ns = 0;
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (long i = 0; i < iterations; ++i) {
            response res(200);
            res.content(body, "application/json");
            std::string out;
            http1_protocol::serialise(res, false, true, host, out);
            host.sink.clear();
        }
        ser_ns = seconds_since(t0) * 1e9 / double(iterations);
    }

    // --------------------------------------- the whole thing, as a connection
    double total_ns = 0;
    {
        request_parser p;                       // reused, as a live connection does
        const auto t0 = std::chrono::steady_clock::now();
        for (long i = 0; i < iterations; ++i) {
            std::size_t used = 0;
            p.parse(wire.data(), wire.size(), used);
            std::unordered_map<std::string, std::string> params;
            bool exists = false;
            std::string allowed;
            routes.match(p.message(), params, exists, allowed);
            response res(200);
            res.content(body, "application/json");
            std::string out;
            http1_protocol::serialise(res, false, true, host, out);
            p.reset();
        }
        total_ns = seconds_since(t0) * 1e9 / double(iterations);
    }

    if (markdown)
        std::printf("### `make bench-request` - CPU cost of one request\n\n"
                    "| Stage | Per request | Rate | vs total |\n|---|---|---|---|\n");
    else
        std::printf("request path - %d headers, %d routes, no sockets\n\n", extra_headers, n_routes);

    row("parse (cold: first on a connection)", parse_cold_ns, 0);
    row("parse (warm: keep-alive steady state)", parse_warm_ns, 0);
    row("route", route_ns, 0);
    row("serialise", ser_ns, 0);
    row("full request, keep-alive", total_ns, 0);

    if (!markdown)
        std::printf("\nAt %.0f ns of CPU per request, one core tops out near %.0f k req/s\n"
                    "before a single syscall is paid for.\n", total_ns, 1e6 / total_ns);
    return 0;
}

#endif
