//
//  webhook_ws_hub_demo.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - a webhook -> WebSocket broadcast hub, demonstrated
//
//  One server, one event loop. On it we mount snicholls::http::ws_broadcast_hub:
//  a POST endpoint that any webhook source can call, and a WebSocket endpoint a
//  crowd of browsers subscribe to. A message posted to /ingest/<topic> is fanned
//  out live to every WebSocket subscriber of that topic, plus the wildcard
//  firehose, and a small per-topic history is replayed to whoever connects next.
//
//  The hub knows nothing about what the messages mean - topics are strings and
//  payloads are bytes - which is the whole point: drop it in front of any source.
//
//  Every scenario verifies what it claims and the program exits non-zero if any
//  check fails, so this doubles as an integration test that prints a table.
//
//  Build and run:   make demo-webhook            (compiled -O3 -DNDEBUG)
//

#include "../TSMoveables/http/ws_broadcast_hub.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if SNICHOLLS_HAS_WS_BROADCAST_HUB

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace snicholls;
using namespace snicholls::http;
using namespace std::chrono_literals;

namespace {

bool markdown = false;

void check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::exit(1);
    }
}

void row(const char* what, const char* result)
{
    if (markdown)
        std::printf("| %s | %s |\n", what, result);
    else
        std::printf("  %-42s %s\n", what, result);
}

double seconds_since(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// A minimal RFC 6455 client: masks what it sends, reads unmasked server frames
class ws_client {
public:
    bool connect_to(std::uint16_t port, const char* path)
    {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0)
            return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0)
            return false;
        const int on = 1;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
        timeval tv{};
        tv.tv_sec = 5;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

        const std::string req =
            std::string("GET ") + path + " HTTP/1.1\r\nHost: t\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n";
        send_raw(req);

        std::string head;
        while (head.find("\r\n\r\n") == std::string::npos) {
            char t[1024];
            const ssize_t n = ::recv(fd_, t, sizeof t, 0);
            if (n <= 0)
                return false;
            head.append(t, std::size_t(n));
        }
        const std::size_t end = head.find("\r\n\r\n") + 4;
        buf_.assign(head, end, std::string::npos);
        return std::atoi(head.c_str() + 9) == 101;
    }

    ~ws_client() { if (fd_ >= 0) ::close(fd_); }
    ws_client() = default;
    ws_client(ws_client&& o) noexcept : fd_(o.fd_), buf_(std::move(o.buf_)) { o.fd_ = -1; }
    ws_client(const ws_client&) = delete;
    ws_client& operator=(const ws_client&) = delete;

    void send_raw(const std::string& s)
    {
        std::size_t off = 0;
        while (off < s.size()) {
            const ssize_t n = ::send(fd_, s.data() + off, s.size() - off, 0);
            if (n <= 0)
                return;
            off += std::size_t(n);
        }
    }

    // Read one text/binary frame, skipping control frames; false on close/timeout
    bool read_data_frame(std::string& payload)
    {
        for (;;) {
            ws_opcode op;
            if (!read_frame(op, payload))
                return false;
            if (op == ws_opcode::text || op == ws_opcode::binary)
                return true;
            if (op == ws_opcode::close)
                return false;
        }
    }

private:
    bool read_frame(ws_opcode& op, std::string& payload)
    {
        for (;;) {
            if (buf_.size() >= 2) {
                const unsigned char* p = reinterpret_cast<const unsigned char*>(buf_.data());
                op = static_cast<ws_opcode>(p[0] & 0x0f);
                std::uint64_t len = p[1] & 0x7f;
                std::size_t hdr = 2;
                if (len == 126 && buf_.size() >= 4) {
                    len = (std::uint64_t(p[2]) << 8) | p[3];
                    hdr = 4;
                } else if (len == 127 && buf_.size() >= 10) {
                    len = 0;
                    for (int i = 0; i < 8; ++i) len = (len << 8) | p[2 + i];
                    hdr = 10;
                }
                if (buf_.size() >= hdr + len) {
                    payload.assign(buf_, hdr, std::size_t(len));
                    buf_.erase(0, hdr + std::size_t(len));
                    return true;
                }
            }
            char t[65536];
            const ssize_t n = ::recv(fd_, t, sizeof t, 0);
            if (n <= 0)
                return false;
            buf_.append(t, std::size_t(n));
        }
    }

    int fd_ = -1;
    std::string buf_;
};

// One blocking HTTP POST; returns the status code (or -1)
int http_post(std::uint16_t port, const std::string& path, const std::string& body)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        ::close(fd);
        return -1;
    }
    const std::string req = "POST " + path + " HTTP/1.1\r\nHost: t\r\nConnection: close\r\n"
                            "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    std::size_t off = 0;
    while (off < req.size()) {
        const ssize_t n = ::send(fd, req.data() + off, req.size() - off, 0);
        if (n <= 0) { ::close(fd); return -1; }
        off += std::size_t(n);
    }
    std::string resp;
    char t[4096];
    for (;;) {
        const ssize_t n = ::recv(fd, t, sizeof t, 0);
        if (n <= 0)
            break;
        resp.append(t, std::size_t(n));
        if (resp.find("\r\n\r\n") != std::string::npos)
            break;
    }
    ::close(fd);
    return resp.size() >= 12 ? std::atoi(resp.c_str() + 9) : -1;
}

bool contains(const std::string& hay, const std::string& needle)
{
    return hay.find(needle) != std::string::npos;
}

template <typename Pred>
void spin_until(Pred pred)
{
    while (!pred())
        std::this_thread::yield();
}

} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--markdown") == 0)
            markdown = true;

    if (markdown)
        std::printf("### `make demo-webhook` - webhook to WebSocket broadcast hub\n\n"
                    "| Scenario | Result |\n|---|---|\n");
    else
        std::printf("webhook -> websocket broadcast hub demo\n\n");

    char buf[192];

    server srv;
    ws_broadcast_hub hub;
    hub.mount(srv);                                     // GET /ws?topic=.. + POST /ingest/:topic

    const std::uint16_t port = srv.listen("127.0.0.1", 0);
    std::thread loop_thread([&] { srv.run(); });
    while (!srv.running())
        std::this_thread::yield();

    // ---------------------------------------- 1. one webhook, one live subscriber
    {
        ws_client c;
        check(c.connect_to(port, "/ws?topic=prices"), "subscriber connected to /ws?topic=prices");
        spin_until([&] { return hub.subscribers() == 1; });

        check(http_post(port, "/ingest/prices", "{\"BTC\":64000}") == 202, "webhook POST accepted (202)");

        std::string got;
        check(c.read_data_frame(got), "subscriber received a frame");
        check(contains(got, "\"topic\":\"prices\""), "frame carries the topic");
        check(contains(got, "\"payload\":\"{\\\"BTC\\\":64000}\""), "frame carries the escaped payload");
        check(contains(got, "\"seq\":"), "frame carries a sequence number");
        row("webhook POST -> live WebSocket delivery", "one payload framed and delivered");
    }

    // -------------------------------------------------- 2. fan-out to many readers
    {
        const int n = 50;
        std::vector<ws_client> clients(n);
        for (int i = 0; i < n; ++i)
            check(clients[i].connect_to(port, "/ws?topic=room"), "fan-out subscriber connected");
        spin_until([&] { return hub.subscribers() == std::size_t(n + 1); });   // +1: the prices sub above

        const std::size_t fanned = hub.publish("room", "everyone-sees-this");
        check(fanned == std::size_t(n), "publish() reports the fan-out count");

        int received = 0;
        for (int i = 0; i < n; ++i) {
            std::string got;
            if (clients[i].read_data_frame(got) && contains(got, "everyone-sees-this"))
                ++received;
        }
        check(received == n, "every fan-out subscriber received the message");
        std::snprintf(buf, sizeof buf, "1 publish -> %d subscribers, all delivered", n);
        row("fan-out to many subscribers", buf);
    }

    // ------------------------------------------------------ 3. replay on connect
    {
        // Post history to a brand-new topic before anyone is listening
        check(http_post(port, "/ingest/log", "line-1") == 202, "history line 1 posted");
        check(http_post(port, "/ingest/log", "line-2") == 202, "history line 2 posted");
        check(http_post(port, "/ingest/log", "line-3") == 202, "history line 3 posted");

        ws_client late;
        check(late.connect_to(port, "/ws?topic=log"), "late subscriber connected");
        std::string a, b, c;
        check(late.read_data_frame(a) && contains(a, "line-1"), "replay delivered line 1");
        check(late.read_data_frame(b) && contains(b, "line-2"), "replay delivered line 2");
        check(late.read_data_frame(c) && contains(c, "line-3"), "replay delivered line 3 (in order)");

        check(http_post(port, "/ingest/log", "line-4") == 202, "live line 4 posted");
        std::string d;
        check(late.read_data_frame(d) && contains(d, "line-4"), "live message followed the replay");
        row("replay-on-connect then live", "3 replayed + 1 live, in order");
    }

    // --------------------------------------------------- 4. wildcard firehose
    {
        ws_client fire;
        check(fire.connect_to(port, "/ws?topic=*"), "wildcard subscriber connected");
        spin_until([&] { return hub.subscribers() >= 1; });

        // A wildcard client first replays the recent history of every topic (all
        // prior scenarios flowed through the wildcard ring), then sees new ones
        // live. Post to a topic that never existed and drain until it arrives.
        check(http_post(port, "/ingest/anything", "wild") == 202, "posted to an arbitrary topic");
        std::string got;
        bool saw = false;
        for (int i = 0; i < 1000 && fire.read_data_frame(got); ++i)
            if (contains(got, "\"topic\":\"anything\"") && contains(got, "wild")) { saw = true; break; }
        check(saw, "wildcard sees a topic it never subscribed to by name");
        row("wildcard firehose subscriber", "receives all topics regardless of name");
    }

    // ------------------------------------------------------------ 5. throughput
    {
        ws_client c;
        check(c.connect_to(port, "/ws?topic=bench"), "throughput subscriber connected");
        spin_until([&] { return hub.subscribers() >= 1; });

        const int messages = 20000;
        std::atomic<int> received{0};
        std::thread reader([&] {
            std::string got;
            for (int i = 0; i < messages; ++i) {
                if (!c.read_data_frame(got))
                    break;
                received.fetch_add(1, std::memory_order_relaxed);
            }
        });

        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < messages; ++i)
            hub.publish("bench", "x");
        spin_until([&] { return received.load() >= messages; });
        const double secs = seconds_since(t0);
        reader.join();

        check(received.load() == messages, "every throughput message was delivered");
        std::snprintf(buf, sizeof buf, "%d msgs in %.0f ms = %.0f msg/s (1 publisher, 1 subscriber)",
                      messages, secs * 1000.0, double(messages) / secs);
        row("publish -> deliver throughput", buf);
    }

    const auto st = hub.snapshot();
    std::snprintf(buf, sizeof buf, "%llu published, %llu delivered, %llu dropped, %llu live subs",
                  (unsigned long long)st.published, (unsigned long long)st.delivered,
                  (unsigned long long)st.dropped, (unsigned long long)st.subscribers);
    row("hub totals", buf);
    row("server threads", "1 - a single event_loop served every subscriber");

    srv.stop();
    loop_thread.join();

    if (!markdown)
        std::printf("\nall webhook -> websocket hub demo checks passed\n");
    return 0;
}

#else // !SNICHOLLS_HAS_WS_BROADCAST_HUB

int main()
{
    std::printf("webhook ws hub demo: WebSocket support is POSIX-only in phase 1 - demo skipped\n");
    return 0;
}

#endif
