//
//  ws_tls_throughput.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - what does wss:// cost against ws://?
//
//  The same server, the same client, the same messages, twice: once in
//  plaintext and once through the OpenSSL transport, so the only difference in
//  the number is TLS. It is reported as a ratio rather than two rates, because
//  the absolute rate is this machine's loopback and scheduler and the ratio is
//  the part that travels.
//
//      make bench-wss
//
//  Swept by payload size, because TLS has two costs that behave differently: a
//  fixed per-record overhead (header, MAC, padding) and per-byte symmetric
//  encryption. The obvious prediction is that the fixed part dominates at small
//  payloads and so the ratio should be worst there.
//
//  It is not what this measures. The ratio is best at small payloads and WORST
//  at the largest - 0.88 at 256 B against 0.58 at 16 KB - so what costs here is
//  per-byte encryption, not the fixed per-record overhead one would expect to
//  dominate small messages. The sweep is kept because that is how the
//  prediction was falsified; a benchmark reporting only the axis where the
//  guess held would not have told anyone.
//
//  Verification is left ON and a throwaway certificate is generated and
//  trusted, rather than reaching for insecure_skip_verify to save a temporary
//  file. The handshake happens once and is not in the measured window, so
//  turning verification off would not move these numbers - it would only make
//  the benchmark exercise a configuration nobody should deploy.
//

#include "../../TSMoveables/http/websocket_client.hpp"
#include "../../TSMoveables/http/websocket.hpp"
#include "../../TSMoveables/tls/openssl.hpp"
#include "../../TSMoveables/moveable/mutex.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if !SNICHOLLS_HAS_WEBSOCKET_CLIENT

int main()
{
    std::printf("ws/wss throughput: POSIX only - skipped\n");
    return 0;
}

#else

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

using namespace snicholls;
using namespace snicholls::http;

namespace {

bool markdown = false;

// ------------------------------------------------ a throwaway certificate
//
// CN=localhost, self-signed, so it is its own CA and the client can be given
// the same PEM as a trust anchor. Lives only as long as the process.

struct throwaway_cert {
    EVP_PKEY* key = nullptr;
    X509* cert = nullptr;
    std::string pem_path;

    throwaway_cert()
    {
        key = EVP_RSA_gen(2048);
        cert = X509_new();
        X509_set_version(cert, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
        X509_gmtime_adj(X509_getm_notBefore(cert), 0);
        X509_gmtime_adj(X509_getm_notAfter(cert), 60L * 60);
        X509_set_pubkey(cert, key);
        X509_NAME* n = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(n, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
        X509_set_issuer_name(cert, n);
        if (X509_sign(cert, key, EVP_sha256()) <= 0)
            std::exit(1);

        char dir[] = "/tmp/tsm_wssbenchXXXXXX";
        if (!::mkdtemp(dir))
            std::exit(1);
        pem_path = std::string(dir) + "/ca.pem";
        BIO* b = BIO_new(BIO_s_mem());
        PEM_write_bio_X509(b, cert);
        char* p = nullptr;
        const long len = BIO_get_mem_data(b, &p);
        std::ofstream(pem_path).write(p, len);
        BIO_free(b);
    }

    ~throwaway_cert()
    {
        if (cert) X509_free(cert);
        if (key) EVP_PKEY_free(key);
    }
};

double seconds_since(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

struct result {
    std::size_t payload = 0;
    std::uint64_t msgs = 0;
    double secs = 0;
    double msgs_s = 0;
    double mb_s = 0;
};

// Server pushes `msgs` frames at a connected client and we time how long the
// client takes to receive them all. That is the relay's real direction:
// upstream in, subscribers out, and the bytes only ever go one way per hop.
result run_case(bool secure, const throwaway_cert& cred, std::size_t payload, std::uint64_t msgs)
{
    server srv;
    openssl_context tls;
    if (secure) {
        tls.use_certificate_and_key(cred.cert, cred.key);
        tls.set_alpn({"http/1.1"});
        srv.transport_factory(tls.factory());
    }

    std::vector<websocket> live;
    moveable_mutex<> live_mtx;
    srv.get("/feed", websocket_route([&](websocket ws) {
        std::lock_guard<moveable_mutex<>> g(live_mtx);
        live.push_back(ws);
    }));
    const std::uint16_t port = srv.listen("127.0.0.1", 0);
    if (!port)
        std::exit(1);
    std::thread srv_thread([&srv] { srv.run(); });
    while (!srv.running())
        std::this_thread::yield();

    event_loop loop;
    std::atomic<std::uint64_t> got{0};

    ws_client_config cfg;
    cfg.auto_reconnect = false;
    std::shared_ptr<openssl_client_context> ctls;
    if (secure) {
        openssl_client_config c;
        c.ca_file = cred.pem_path;              // trust exactly this certificate
        ctls = std::make_shared<openssl_client_context>(c);
        cfg.transport_factory = [ctls](const std::string& host) {
            return std::unique_ptr<transport_delegate>(ctls->connect(host).release());
        };
    }

    websocket_client c{cfg};
    c.on_message([&got](const ws_message&) { got.fetch_add(1, std::memory_order_relaxed); });

    const std::string scheme = secure ? "wss://localhost:" : "ws://localhost:";
    if (!c.connect(loop, scheme + std::to_string(port) + "/feed"))
        std::exit(1);
    std::thread loop_thread([&loop] { loop.run(); });
    while (!loop.running())
        std::this_thread::yield();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    for (;;) {
        std::size_t n = 0;
        { std::lock_guard<moveable_mutex<>> g(live_mtx); n = live.size(); }
        if ((n == 1 && c.connected()) || std::chrono::steady_clock::now() > deadline)
            break;
        std::this_thread::yield();
    }

    // Paced, with a bounded amount in flight.
    //
    // The first version of this pushed every message before reading any, which
    // at 4 KB meant shoving 32 MB into a connection nobody was draining. What
    // it then timed was the write path backing up, not the transport: rates
    // collapsed sixtyfold between 1 KB and 4 KB, and at 16 KB wss came out
    // FASTER than ws - which is impossible and is what gave it away. A
    // benchmark whose ratio crosses 1.0 in the direction that cannot happen is
    // measuring itself.
    //
    // So: never let more than a window's worth be outstanding, and measure the
    // steady state that produces.
    static const std::uint64_t kInFlightBytes = 1u << 20;      // 1 MiB
    const std::uint64_t window =
        std::max<std::uint64_t>(8, kInFlightBytes / std::max<std::size_t>(payload, 1));

    const std::string body(payload, 'x');
    const auto t0 = std::chrono::steady_clock::now();
    const auto hard = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    std::uint64_t pushed = 0;
    while (pushed < msgs && std::chrono::steady_clock::now() < hard) {
        const std::uint64_t behind = pushed - got.load(std::memory_order_relaxed);
        if (behind >= window) {
            std::this_thread::yield();
            continue;
        }
        const std::uint64_t batch = std::min<std::uint64_t>(window - behind, msgs - pushed);
        std::lock_guard<moveable_mutex<>> g(live_mtx);
        for (std::uint64_t i = 0; i < batch; ++i)
            live[0].send_text(body);
        pushed += batch;
    }
    while (got.load(std::memory_order_relaxed) < msgs &&
           std::chrono::steady_clock::now() < hard)
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    const double secs = seconds_since(t0);
    const std::uint64_t received = got.load();

    c.close();
    loop.stop();
    loop_thread.join();
    srv.stop();
    srv_thread.join();

    result r;
    r.payload = payload;
    r.msgs = received;
    r.secs = secs;
    const double s = secs > 0 ? secs : 1;
    r.msgs_s = double(received) / s;
    r.mb_s = double(received) * double(payload) / s / (1024.0 * 1024.0);
    return r;
}

} // namespace

int main(int argc, char** argv)
{
    std::uint64_t msgs = 20000;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--markdown") == 0) markdown = true;
        else if (std::strcmp(argv[i], "--msgs") == 0 && i + 1 < argc)
            msgs = std::strtoull(argv[++i], nullptr, 10);
    }

    throwaway_cert cred;
    const std::size_t sweep[] = {64, 256, 1024, 4096, 16384};

    if (markdown) {
        std::printf("### `make bench-wss` - what TLS costs on the WebSocket path\n\n");
        std::printf("| payload | ws msg/s | wss msg/s | ws MB/s | wss MB/s | wss/ws |\n");
        std::printf("|---|---|---|---|---|---|\n");
    } else {
        std::printf("ws vs wss - %llu messages per case, one connection, loopback\n\n",
                    (unsigned long long)msgs);
        std::printf("  %-9s %12s %12s %10s %10s %8s\n",
                    "payload", "ws msg/s", "wss msg/s", "ws MB/s", "wss MB/s", "wss/ws");
    }

    for (std::size_t p : sweep) {
        // Constant bytes rather than constant messages: 20k x 16 KB is 320 MB
        // and 20k x 64 B is 1.2 MB, which are not comparable amounts of work.
        const std::uint64_t n = std::max<std::uint64_t>(2000, msgs * 256 / p);
        const result plain  = run_case(false, cred, p, n);
        const result secure = run_case(true,  cred, p, n);
        const double ratio = plain.msgs_s > 0 ? secure.msgs_s / plain.msgs_s : 0.0;
        if (markdown)
            std::printf("| %zu B | %.0f | %.0f | %.1f | %.1f | **%.2f×** |\n",
                        p, plain.msgs_s, secure.msgs_s, plain.mb_s, secure.mb_s, ratio);
        else
            std::printf("  %-9zu %12.0f %12.0f %10.1f %10.1f %7.2fx\n",
                        p, plain.msgs_s, secure.msgs_s, plain.mb_s, secure.mb_s, ratio);
    }

    const char* note =
        "\nThe ratio is best at small payloads and worst at the largest, so what costs\n"
        "LARGEST payload, not the smallest - so what costs here is per-byte encryption\n"
        "rather than the fixed per-record overhead one would expect to dominate small\n"
        "messages. Read the ratio, not the rate: the rate is this machine's loopback.\n"
        "Verification stays on\n"
        "and a throwaway certificate is trusted properly - the handshake is outside the\n"
        "measured window, so weakening it would change nothing here except what the\n"
        "benchmark demonstrates.\n";
    std::printf("%s", note);
    return 0;
}

#endif
