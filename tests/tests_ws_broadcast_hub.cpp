//
//  tests_ws_broadcast_hub.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - unit tests for the webhook -> WebSocket broadcast hub
//
//  A real server on loopback, driven by a hand-rolled WebSocket client (to
//  receive) and a blocking HTTP client (to POST into the ingest side). The
//  claims under test: fan-out reaches every subscriber, a topic isolates its
//  subscribers from other topics, the wildcard sees everything, replay-on-
//  connect hands a fresh client the recent history in order, a graceful close
//  unsubscribes, the direct publish() API works, raw framing passes bytes
//  through untouched, and the per-connection queue is bounded (oldest-dropped)
//  under backpressure.
//
//  And the origin policy on both doors: a foreign Origin cannot upgrade /ws or
//  publish to /ingest, while the callers that must keep working - a loopback
//  viewer, an allowlisted app, a native client that sends no Origin at all -
//  are untouched.
//

#include "test_helpers.hpp"

#include "../TSMoveables/http/ws_broadcast_hub.hpp"

#if SNICHOLLS_HAS_WS_BROADCAST_HUB

#include <atomic>
#include <chrono>
#include <cstring>
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

// ------------------------------------------------------------- a ws client
// The same minimal RFC 6455 client the websocket tests use: masks its frames,
// reads unmasked server frames, and reassembles nothing beyond one frame.

class ws_client {
public:
    // `origin`, when given, is sent as the Origin header - which is how a
    // browser identifies the page opening the socket, and the only thing the
    // handshake has to go on. nullptr means no header at all: a curl or native
    // client, which is a different case from an empty one.
    bool connect_to(std::uint16_t port, const char* path, const char* origin = nullptr)
    {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        assert(fd_ >= 0);
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

        std::string req =
            std::string("GET ") + path + " HTTP/1.1\r\nHost: t\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n";
        if (origin)
            req += std::string("Origin: ") + origin + "\r\n";
        req += "\r\n";
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
        status_ = std::atoi(head.c_str() + 9);
        return true;
    }

    ~ws_client() { close(); }
    ws_client() = default;
    ws_client(ws_client&& o) noexcept : fd_(o.fd_), status_(o.status_), buf_(std::move(o.buf_)) { o.fd_ = -1; }
    ws_client(const ws_client&) = delete;
    ws_client& operator=(const ws_client&) = delete;

    void close()
    {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    int status() const noexcept { return status_; }

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

    void send_frame(ws_opcode op, const std::string& payload, bool fin = true)
    {
        std::string f;
        f.push_back(char((fin ? 0x80 : 0x00) | static_cast<std::uint8_t>(op)));
        const std::size_t n = payload.size();
        const char m = char(0x80);                      // client frames are masked
        if (n < 126) {
            f.push_back(char(m | char(n)));
        } else if (n <= 0xFFFF) {
            f.push_back(char(m | char(126)));
            f.push_back(char((n >> 8) & 0xff));
            f.push_back(char(n & 0xff));
        } else {
            f.push_back(char(m | char(127)));
            for (int i = 7; i >= 0; --i)
                f.push_back(char((std::uint64_t(n) >> (i * 8)) & 0xff));
        }
        const unsigned char key[4] = {0x37, 0xfa, 0x21, 0x3d};
        for (int i = 0; i < 4; ++i)
            f.push_back(char(key[i]));
        for (std::size_t i = 0; i < n; ++i)
            f.push_back(char(static_cast<unsigned char>(payload[i]) ^ key[i & 3]));
        send_raw(f);
    }

    // Read one server frame; false on timeout/close
    bool read_frame(ws_opcode& op, std::string& payload)
    {
        bool fin = false;
        while (!parse(op, payload, fin)) {
            char t[65536];
            const ssize_t n = ::recv(fd_, t, sizeof t, 0);
            if (n <= 0)
                return false;
            buf_.append(t, std::size_t(n));
        }
        return true;
    }

    // Read one data (text/binary) frame, skipping ping/pong/close control frames
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
    bool parse(ws_opcode& op, std::string& payload, bool& fin)
    {
        if (buf_.size() < 2)
            return false;
        const unsigned char* p = reinterpret_cast<const unsigned char*>(buf_.data());
        fin = (p[0] & 0x80) != 0;
        op = static_cast<ws_opcode>(p[0] & 0x0f);
        std::uint64_t len = p[1] & 0x7f;
        std::size_t hdr = 2;
        if (len == 126) {
            if (buf_.size() < 4) return false;
            len = (std::uint64_t(p[2]) << 8) | p[3];
            hdr = 4;
        } else if (len == 127) {
            if (buf_.size() < 10) return false;
            len = 0;
            for (int i = 0; i < 8; ++i)
                len = (len << 8) | p[2 + i];
            hdr = 10;
        }
        if (buf_.size() < hdr + len)
            return false;
        payload.assign(buf_, hdr, std::size_t(len));
        buf_.erase(0, hdr + std::size_t(len));
        return true;
    }

    int fd_ = -1;
    int status_ = 0;
    std::string buf_;
};

// A one-shot blocking HTTP POST, returns the status code (or -1)
int http_post(std::uint16_t port, const std::string& path, const std::string& body,
              const char* origin = nullptr)
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
    std::string req = "POST " + path + " HTTP/1.1\r\nHost: t\r\nConnection: close\r\n"
                      "Content-Type: application/octet-stream\r\nContent-Length: " +
                      std::to_string(body.size()) + "\r\n";
    if (origin)
        req += std::string("Origin: ") + origin + "\r\n";
    req += "\r\n" + body;
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

// A hub mounted on a loopback server, torn down cleanly on destruction
struct hub_server {
    explicit hub_server(ws_broadcast_hub::config cfg = ws_broadcast_hub::config{})
        : hub(std::move(cfg))
    {
        hub.mount(srv);
        port = srv.listen("127.0.0.1", 0);
        th = std::thread([this] { srv.run(); });
        spin_until([this] { return srv.running(); });
    }
    ~hub_server()
    {
        srv.stop();
        if (th.joinable())
            th.join();
    }

    server            srv;
    ws_broadcast_hub  hub;
    std::uint16_t     port = 0;
    std::thread       th;
};

bool contains(const std::string& hay, const std::string& needle)
{
    return hay.find(needle) != std::string::npos;
}

// ------------------------------------------------------------------- tests

void test_hub_ingest_and_deliver()
{
    hub_server s;
    ws_client c;
    assert(c.connect_to(s.port, "/ws?topic=alerts"));
    assert(c.status() == 101);
    spin_until([&] { return s.hub.subscribers() == 1; });

    assert(http_post(s.port, "/ingest/alerts", "hello world") == 202);

    std::string got;
    assert(c.read_data_frame(got));
    assert(contains(got, "\"topic\":\"alerts\""));
    assert(contains(got, "\"payload\":\"hello world\""));
    assert(contains(got, "\"seq\":1"));
    assert(contains(got, "\"ts_ms\":"));

    pass("ws_broadcast_hub: ingest a webhook, deliver a framed envelope");
}

void test_hub_topic_isolation()
{
    hub_server s;
    ws_client alerts, news;
    assert(alerts.connect_to(s.port, "/ws?topic=alerts"));
    assert(news.connect_to(s.port, "/ws?topic=news"));
    spin_until([&] { return s.hub.subscribers() == 2; });

    assert(http_post(s.port, "/ingest/news", "breaking") == 202);

    std::string got;
    assert(news.read_data_frame(got));
    assert(contains(got, "\"payload\":\"breaking\""));

    // The alerts subscriber must not receive the news message. Prove it by
    // posting to alerts and checking that is the FIRST thing it sees.
    assert(http_post(s.port, "/ingest/alerts", "for-alerts") == 202);
    std::string a;
    assert(alerts.read_data_frame(a));
    assert(contains(a, "\"payload\":\"for-alerts\""));
    assert(!contains(a, "breaking"));

    pass("ws_broadcast_hub: a topic isolates its subscribers");
}

void test_hub_wildcard_and_fanout()
{
    hub_server s;
    ws_client star, a1, a2;
    assert(star.connect_to(s.port, "/ws?topic=*"));     // firehose
    assert(a1.connect_to(s.port, "/ws?topic=room"));
    assert(a2.connect_to(s.port, "/ws?topic=room"));
    spin_until([&] { return s.hub.subscribers() == 3; });

    assert(http_post(s.port, "/ingest/room", "party") == 202);

    std::string g1, g2, gs;
    assert(a1.read_data_frame(g1));
    assert(a2.read_data_frame(g2));
    assert(star.read_data_frame(gs));
    assert(contains(g1, "\"payload\":\"party\""));
    assert(contains(g2, "\"payload\":\"party\""));
    assert(contains(gs, "\"payload\":\"party\""));          // wildcard saw it too
    assert(contains(gs, "\"topic\":\"room\""));

    pass("ws_broadcast_hub: wildcard subscriber + fan-out to many");
}

void test_hub_replay_on_connect()
{
    hub_server s;
    // Publish history BEFORE anyone is listening
    assert(http_post(s.port, "/ingest/log", "first") == 202);
    assert(http_post(s.port, "/ingest/log", "second") == 202);
    assert(http_post(s.port, "/ingest/log", "third") == 202);

    // A fresh client sees the recent history immediately, oldest first
    ws_client c;
    assert(c.connect_to(s.port, "/ws?topic=log"));
    std::string a, b, d;
    assert(c.read_data_frame(a));
    assert(c.read_data_frame(b));
    assert(c.read_data_frame(d));
    assert(contains(a, "\"payload\":\"first\""));
    assert(contains(b, "\"payload\":\"second\""));
    assert(contains(d, "\"payload\":\"third\""));

    // ...and live messages continue in order after the replay
    assert(http_post(s.port, "/ingest/log", "fourth") == 202);
    std::string e;
    assert(c.read_data_frame(e));
    assert(contains(e, "\"payload\":\"fourth\""));

    pass("ws_broadcast_hub: replay-on-connect, then live, in order");
}

void test_hub_replay_bounded_by_bytes()
{
    // The ring is bounded by chunk COUNT and by BYTES, and the byte bound is
    // the one that matters under fire: a chunk is whatever one producer
    // batched (up to max_message_bytes), so a count alone is a worst case of
    // ring_capacity x 1MB per topic - found the hard way, as 66MB of live
    // heap in a single topic's ring after a 20-minute soak. Keep the count
    // generous here and prove the bytes stay bounded, the survivors are the
    // NEWEST run, and nothing between them was skipped.
    ws_broadcast_hub::config cfg;
    cfg.ring_capacity = 1024;                        // generous, as a dashboard wants
    cfg.ring_bytes    = 64 * 1024;                   // the budget under test
    hub_server s(cfg);

    const int total = 64;
    const std::size_t chunk = 8 * 1024;              // 64 x 8KB = 8x the budget
    for (int i = 0; i < total; ++i) {
        std::string body = "m" + std::to_string(i) + ":";
        body.resize(chunk, 'x');
        assert(http_post(s.port, "/ingest/fire", body) == 202);
    }

    // Accounted per ring, and every publish feeds two rings (topic + wildcard),
    // so the hub-wide figure is bounded by twice the per-ring budget. The
    // envelope adds bytes around each payload; one chunk of slack absorbs it.
    const auto held = s.hub.snapshot().ring_bytes;
    assert(held > 0);
    assert(held <= 2 * (cfg.ring_bytes + chunk + 256));

    // A fresh client replays only what fit: a contiguous run ending at the
    // newest message, oldest evicted first.
    ws_client c;
    assert(c.connect_to(s.port, "/ws?topic=fire"));
    assert(http_post(s.port, "/ingest/fire", "end") == 202);   // sentinel: replay precedes live

    std::vector<int> replayed;
    for (;;) {
        std::string f;
        assert(c.read_data_frame(f));
        if (contains(f, "\"payload\":\"end\""))
            break;
        const std::size_t at = f.find("\"payload\":\"m");
        assert(at != std::string::npos);
        replayed.push_back(std::atoi(f.c_str() + at + 12));
    }
    assert(!replayed.empty());
    assert(replayed.size() < std::size_t(total));            // something was evicted
    assert(replayed.size() * chunk <= cfg.ring_bytes);       // and what remains fits
    assert(replayed.back() == total - 1);                    // newest survived
    for (std::size_t k = 1; k < replayed.size(); ++k)        // as one gapless run
        assert(replayed[k] == replayed[k - 1] + 1);

    pass("ws_broadcast_hub: replay ring evicts oldest to hold its byte budget");
}

void test_hub_replay_keeps_full_depth_when_small()
{
    // The other half of the pair, and the half a byte budget could quietly
    // break: bounding bytes must not cost a *quiet* topic its history.
    //
    // That is not hypothetical. Before ring_bytes existed the only way to
    // bound the hub's memory was to cut ring_capacity - super-log took it
    // from 1024 to 128 to stop a firehose topic growing - and that bounds
    // bytes only by accident, at the price of the streams that were never
    // the problem: a one-line-per-second producer went from replaying about
    // seventeen minutes on connect to about two. The whole point of bounding
    // the two independently is that a generous count becomes free for quiet
    // topics, so that freedom is worth a test rather than an assumption.
    ws_broadcast_hub::config cfg;
    cfg.ring_capacity = 256;                         // generous, as it may now be
    cfg.ring_bytes    = 8 * 1024 * 1024;             // far above what this publishes
    hub_server s(cfg);

    const int total = 200;                           // under the count, nowhere near the bytes
    for (int i = 0; i < total; ++i)
        assert(http_post(s.port, "/ingest/quiet", "q" + std::to_string(i)) == 202);

    ws_client c;
    assert(c.connect_to(s.port, "/ws?topic=quiet"));
    assert(http_post(s.port, "/ingest/quiet", "end") == 202);   // sentinel: replay precedes live

    std::vector<int> replayed;
    for (;;) {
        std::string f;
        assert(c.read_data_frame(f));
        if (contains(f, "\"payload\":\"end\""))
            break;
        const std::size_t at = f.find("\"payload\":\"q");
        assert(at != std::string::npos);
        replayed.push_back(std::atoi(f.c_str() + at + 12));
    }

    // Every message, oldest first, none evicted: the byte budget was never
    // reached, so the count is the only bound that applied.
    assert(replayed.size() == std::size_t(total));
    assert(replayed.front() == 0);
    assert(replayed.back() == total - 1);
    for (std::size_t k = 1; k < replayed.size(); ++k)
        assert(replayed[k] == replayed[k - 1] + 1);

    pass("ws_broadcast_hub: a byte budget costs a quiet topic no replay depth");
}

void test_hub_unsubscribe_on_close()
{
    hub_server s;
    {
        ws_client c;
        assert(c.connect_to(s.port, "/ws?topic=temp"));
        spin_until([&] { return s.hub.subscribers() == 1; });
        // Graceful close handshake
        c.send_frame(ws_opcode::close, std::string("\x03\xe8", 2));   // 1000
        ws_opcode op;
        std::string ignored;
        c.read_frame(op, ignored);                  // read the close echo back
    }
    // The graceful close unsubscribes us
    assert(spin_until_for([&] { return s.hub.subscribers() == 0; }));

    // Publishing to nobody is fine
    assert(http_post(s.port, "/ingest/temp", "nobody home") == 202);

    pass("ws_broadcast_hub: graceful close unsubscribes");
}

void test_hub_direct_publish_api()
{
    hub_server s;
    ws_client c;
    assert(c.connect_to(s.port, "/ws?topic=api"));
    spin_until([&] { return s.hub.subscribers() == 1; });

    // publish() straight from this (foreign) thread; the mounted hub marshals
    const std::size_t n = s.hub.publish("api", "direct");
    assert(n == 1);

    std::string got;
    assert(c.read_data_frame(got));
    assert(contains(got, "\"payload\":\"direct\""));

    // `delivered` is incremented AFTER the frame is handed to the socket, so
    // reading the frame does not imply the counter has moved yet - the loop
    // thread can be preempted between the send and the fetch_add. Asserting it
    // directly passes on a quiet machine and fails under TSan, which is exactly
    // where it did fail. Wait for it rather than race it.
    assert(spin_until_for([&] { return s.hub.snapshot().delivered >= 1; }));
    const auto st = s.hub.snapshot();
    assert(st.published >= 1);
    assert(st.delivered >= 1);

    pass("ws_broadcast_hub: direct publish() from a foreign thread");
}

void test_hub_raw_framing()
{
    ws_broadcast_hub::config cfg;
    cfg.frame_mode = ws_broadcast_hub::framing::raw;    // payload verbatim, no envelope
    hub_server s(cfg);

    ws_client c;
    assert(c.connect_to(s.port, "/ws?topic=raw"));
    spin_until([&] { return s.hub.subscribers() == 1; });

    assert(http_post(s.port, "/ingest/raw", "just-the-bytes") == 202);
    std::string got;
    assert(c.read_data_frame(got));
    assert(got == "just-the-bytes");                    // no envelope at all

    pass("ws_broadcast_hub: raw framing passes payload through verbatim");
}

void test_hub_backpressure_bounded()
{
    // send_high_water 0 => the pump never hands a frame to the socket, so every
    // publish piles into the per-connection queue. With a queue cap of 2, the
    // oldest are dropped and the drop counter climbs - bounded, never unbounded.
    ws_broadcast_hub::config cfg;
    cfg.send_high_water = 0;
    cfg.max_queue_msgs  = 2;
    cfg.replay_on_connect = false;
    hub_server s(cfg);

    ws_client c;
    assert(c.connect_to(s.port, "/ws?topic=slow"));
    spin_until([&] { return s.hub.subscribers() == 1; });

    for (int i = 0; i < 10; ++i)
        assert(http_post(s.port, "/ingest/slow", "m" + std::to_string(i)) == 202);

    assert(spin_until_for([&] { return s.hub.snapshot().dropped >= 8; }));
    auto st = s.hub.snapshot();
    assert(st.published >= 10);
    assert(st.dropped  >= 8);          // 10 queued, cap 2 => at least 8 trimmed

    pass("ws_broadcast_hub: slow client is bounded, oldest dropped");
}


// Order is the guarantee a broadcast lives or dies on, and "four messages
// arrived in order on one socket" does not establish it. What has to hold at
// volume, across every subscriber at once, is:
//
//   1. seq is strictly increasing and GAPLESS per subscriber - a gap means a
//      silent drop, a repeat or reorder means the fan-out lost its place;
//   2. every subscriber observes the SAME total order, not merely a sorted one.
//      Per-subscriber queues are independent, so this is the property that
//      would break first if fan-out ever went concurrent per subscriber.
void test_hub_total_order_is_identical_for_every_subscriber()
{
    static const int kMessages = 300;
    static const int kSubs = 8;

    hub_server s;
    std::vector<ws_client> subs(kSubs);
    for (int i = 0; i < kSubs; ++i) {
        assert(subs[std::size_t(i)].connect_to(s.port, "/ws?topic=ord"));
        assert(subs[std::size_t(i)].status() == 101);
    }
    spin_until([&] { return s.hub.subscribers() == kSubs; });

    for (int i = 0; i < kMessages; ++i)
        assert(http_post(s.port, "/ingest/ord", "m" + std::to_string(i)) == 202);

    // Read every subscriber's stream in full, recording the sequence it saw
    std::vector<std::vector<std::uint64_t>> seen(kSubs);
    for (int i = 0; i < kSubs; ++i) {
        for (int k = 0; k < kMessages; ++k) {
            std::string frame;
            assert(subs[std::size_t(i)].read_data_frame(frame));
            const std::size_t at = frame.find("\"seq\":");
            assert(at != std::string::npos);
            seen[std::size_t(i)].push_back(std::strtoull(frame.c_str() + at + 6, nullptr, 10));
        }
    }

    // 1. strictly increasing, and consecutive - no gaps, no repeats
    for (int i = 0; i < kSubs; ++i) {
        const auto& v = seen[std::size_t(i)];
        assert(v.size() == std::size_t(kMessages));
        for (std::size_t k = 1; k < v.size(); ++k)
            assert(v[k] == v[k - 1] + 1);
    }

    // 2. and it is the same order for everyone, not just a sorted one each
    for (int i = 1; i < kSubs; ++i)
        assert(seen[std::size_t(i)] == seen[0]);

    // Nothing was quietly dropped to achieve it
    assert(s.hub.snapshot().dropped == 0);

    pass("ws_broadcast_hub: one total order, gapless, identical across 8 subscribers");
}


// The previous test publishes from one thread, which is the case that cannot
// fail. This is the one that can: many threads calling publish() at once, which
// is exactly how a relay with N inbound feeds behaves.
//
// Concurrency makes the interleaving nondeterministic - which message gets seq
// 7 is a race, and that is fine. What must NOT be nondeterministic is the
// order once assigned. Three things are proved here, and together they are
// what "the hub preserves order" has to mean:
//
//   1. seq is a genuine total order: the set delivered is exactly 1..K, so no
//      number was issued twice and none was skipped;
//   2. every subscriber sees it strictly increasing and gapless;
//   3. every subscriber sees the SAME sequence - the identical interleaving,
//      not merely a sorted view of its own.
//
// (3) is the one that would break first: it fails the moment fan-out stops
// being serialised per message, which is precisely the change someone will be
// tempted to make to parallelise the M side.
void test_hub_order_holds_under_concurrent_publishers()
{
    // 32, because that is what this machine has and the race is only as good
    // as the contention it actually creates. CI runners have far fewer cores,
    // where 32 threads oversubscribe and interleave differently - which is a
    // second useful shape for the same invariant, not a weaker one.
    static const int kThreads = 32;
    static const int kPerThread = 60;
    static const int kSubs = 6;
    static const int kTotal = kThreads * kPerThread;

    hub_server s;
    std::vector<ws_client> subs(kSubs);
    for (int i = 0; i < kSubs; ++i)
        assert(subs[std::size_t(i)].connect_to(s.port, "/ws?topic=race"));
    spin_until([&] { return s.hub.subscribers() == kSubs; });

    // Every thread publishes into the same hub at the same time
    std::vector<std::thread> pubs;
    for (int t = 0; t < kThreads; ++t)
        pubs.emplace_back([&s, t] {
            for (int i = 0; i < kPerThread; ++i)
                s.hub.publish("race", "t" + std::to_string(t) + "-" + std::to_string(i));
        });
    for (auto& t : pubs)
        t.join();

    std::vector<std::vector<std::uint64_t>> seen(kSubs);
    for (int i = 0; i < kSubs; ++i) {
        for (int k = 0; k < kTotal; ++k) {
            std::string frame;
            assert(subs[std::size_t(i)].read_data_frame(frame));
            const std::size_t at = frame.find("\"seq\":");
            assert(at != std::string::npos);
            seen[std::size_t(i)].push_back(std::strtoull(frame.c_str() + at + 6, nullptr, 10));
        }
    }

    // 2. gapless and strictly increasing for everyone
    for (int i = 0; i < kSubs; ++i) {
        const auto& v = seen[std::size_t(i)];
        assert(v.size() == std::size_t(kTotal));
        for (std::size_t k = 1; k < v.size(); ++k)
            assert(v[k] == v[k - 1] + 1);
    }

    // 1. the numbers issued are exactly a contiguous run - none duplicated,
    //    none skipped, under a genuine race for the counter
    const std::uint64_t first = seen[0].front();
    for (std::size_t k = 0; k < seen[0].size(); ++k)
        assert(seen[0][k] == first + k);

    // 3. and it is ONE order, seen identically by all of them
    for (int i = 1; i < kSubs; ++i)
        assert(seen[std::size_t(i)] == seen[0]);

    assert(s.hub.snapshot().dropped == 0);

    pass("ws_broadcast_hub: 32 concurrent publishers, one gapless total order, identical for all");
}

// ------------------------------------------------------------------- origin
//
// Browsers do not apply the same-origin policy to WebSockets, so until the
// handshake started reading Origin, any page the developer visited could open
// ws://127.0.0.1:<port>/ws?topic=* against a loopback-bound hub and read every
// stream, replay history included. These tests are that hole, from both sides.

void test_origin_policy_defaults_and_lookalikes()
{
    origin_policy p;                                    // exactly what ships
    auto allows = [&p](const char* o) {
        const std::string s(o);
        return p.allows(&s);
    };

    // No Origin header at all: curl, a native tailer, a webhook sender. Not a
    // hole a page can use - a browser attaches Origin to a cross-origin socket
    // or POST and script cannot take it off.
    assert(p.allows(nullptr));

    // The tool's own console, however it spells the local machine.
    assert(allows("http://localhost"));
    assert(allows("http://localhost:7333"));
    assert(allows("https://localhost:443"));
    assert(allows("http://127.0.0.1:7333"));
    assert(allows("http://127.0.0.2:8080"));            // all of 127/8 is local
    assert(allows("http://[::1]"));
    assert(allows("http://[::1]:7333"));
    assert(allows("HTTP://LOCALHOST:7333"));            // scheme and host are case-blind

    // The drive-by, and every way of dressing it up as local.
    assert(!allows("http://evil.example"));
    assert(!allows("https://evil.example:443"));
    assert(!allows("http://localhost.evil.example"));   // a name its owner controls
    assert(!allows("http://127.0.0.1.evil.example"));
    assert(!allows("http://evil.example/@localhost"));  // has a path: not an origin
    assert(!allows("http://evil.example#localhost"));
    assert(!allows("http://evil.example?x=localhost"));
    assert(!allows("http://localhost@evil.example"));   // userinfo
    assert(!allows("http://1270.0.1"));
    assert(!allows("http://127.0.0.1x"));
    assert(!allows("null"));                            // sandboxed iframe, file://
    assert(!allows(""));                                // a header naming nobody

    // An allowlist entry is one exact serialised origin. A different port or a
    // different scheme is a different origin, and neither is covered.
    p.allow.push_back("https://app.example.com");
    assert(allows("https://app.example.com"));
    assert(allows("HTTPS://APP.EXAMPLE.COM"));
    assert(!allows("https://app.example.com:8443"));
    assert(!allows("http://app.example.com"));
    assert(!allows("https://evil.app.example.com"));

    // allow_if can only widen: it is asked after the built-ins decline, and
    // cannot veto one that already said yes.
    origin_policy q;
    q.allow_loopback = false;
    q.allow_if = [](std::string_view o) { return o == "http://chosen"; };
    const std::string chosen("http://chosen"), local("http://localhost");
    assert(q.allows(&chosen));
    assert(!q.allows(&local));

    // And any() is the one documented way out, for a server that means it.
    origin_policy open = origin_policy::any();
    const std::string evil("http://evil.example");
    assert(open.allows(&evil));
    assert(open.allows(nullptr));

    pass("origin_policy: allows no-Origin, loopback and the list; refuses lookalikes");
}

void test_hub_rejects_foreign_origin_on_ws()
{
    hub_server s;                                       // shipped defaults

    // The reviewer's repro: a raw RFC 6455 handshake carrying a foreign
    // Origin. This used to answer 101 and then stream. Now it is refused
    // before the upgrade, so there is no socket to read.
    ws_client evil;
    assert(evil.connect_to(s.port, "/ws?topic=*", "http://evil.example"));
    assert(evil.status() == 403);

    // And it never became a subscriber, so a publish has nowhere to reach it.
    s.hub.publish("alerts", "secret");
    assert(s.hub.subscribers() == 0);

    pass("ws_broadcast_hub: a foreign Origin is refused 403 before the /ws upgrade");
}

void test_hub_allows_loopback_and_listed_origins()
{
    ws_broadcast_hub::config cfg;
    cfg.origin.allow.push_back("https://app.example.com");
    hub_server s(cfg);

    ws_client local;                                    // the viewer on localhost
    assert(local.connect_to(s.port, "/ws?topic=alerts", "http://localhost:5173"));
    assert(local.status() == 101);

    ws_client native;                                   // a tailer, no Origin
    assert(native.connect_to(s.port, "/ws?topic=alerts"));
    assert(native.status() == 101);

    ws_client listed;                                   // the deployed browser app
    assert(listed.connect_to(s.port, "/ws?topic=alerts", "https://app.example.com"));
    assert(listed.status() == 101);

    spin_until([&] { return s.hub.subscribers() == 3; });

    s.hub.publish("alerts", "hello");
    for (ws_client* c : {&local, &native, &listed}) {
        ws_opcode op;
        std::string payload;
        assert(c->read_frame(op, payload));
        assert(contains(payload, "hello"));
    }

    pass("ws_broadcast_hub: loopback, no-Origin and allowlisted browsers still connect");
}

void test_hub_origin_check_opts_out()
{
    ws_broadcast_hub::config cfg;
    cfg.origin = origin_policy::any();
    hub_server s(cfg);

    ws_client c;
    assert(c.connect_to(s.port, "/ws?topic=alerts", "http://evil.example"));
    assert(c.status() == 101);

    pass("ws_broadcast_hub: origin_policy::any() opts the check out");
}

void test_hub_ingest_rejects_foreign_origin()
{
    hub_server s;

    ws_client sub;
    assert(sub.connect_to(s.port, "/ws?topic=hooks"));  // a tailer: no Origin
    assert(sub.status() == 101);
    spin_until([&] { return s.hub.subscribers() == 1; });

    // The write half of the same drive-by. The page could never read this
    // response, which is exactly why answering it was never the protection -
    // a no-cors text/plain POST is a "simple request", so it crosses with no
    // preflight to refuse and the forged event lands anyway. The Origin it
    // cannot suppress is what stops it.
    assert(http_post(s.port, "/ingest/hooks", "forged", "http://evil.example") == 403);
    assert(s.hub.snapshot().published == 0);

    // The sender that must keep working, unchanged and with no CORS spoken:
    // a webhook POST carries no Origin.
    assert(http_post(s.port, "/ingest/hooks", "real") == 202);

    ws_opcode op;
    std::string payload;
    assert(sub.read_frame(op, payload));
    assert(contains(payload, "real"));
    assert(!contains(payload, "forged"));

    pass("ws_broadcast_hub: a foreign Origin cannot forge an /ingest publish");
}

// A route of your own shadows the hub's guarded one (routes match in
// registration order, first match wins) - and publish() has no request to read
// an Origin from, so it cannot check one for you. Both halves of that are by
// design; what matters is that the one-line fix actually works. A downstream
// consumer shipped the unguarded version of exactly this, so it gets a test.
void test_hub_custom_route_guards_itself()
{
    server srv;
    ws_broadcast_hub hub;

    // Registered BEFORE mount(), so this shadows the hub's /ingest entirely.
    srv.post("/ingest/:topic", [&hub](const request& req, responder res) {
        if (!hub.origin_allowed(req)) {
            res.send(403, "text/plain; charset=utf-8", "403 Forbidden (origin)\n");
            return;
        }
        hub.publish(req.param("topic"), req.body);
        res.send(202, "text/plain; charset=utf-8", "ok\n");
    });
    hub.mount(srv);

    const std::uint16_t port = srv.listen("127.0.0.1", 0);
    std::thread th([&srv] { srv.run(); });
    spin_until([&] { return srv.running(); });

    ws_client sub;
    assert(sub.connect_to(port, "/ws?topic=hooks"));
    assert(sub.status() == 101);
    spin_until([&] { return hub.subscribers() == 1; });

    // The shadowing route is reached, not the hub's - and it refuses.
    assert(http_post(port, "/ingest/hooks", "forged", "http://evil.example") == 403);
    assert(hub.snapshot().published == 0);

    assert(http_post(port, "/ingest/hooks", "real") == 202);
    ws_opcode op;
    std::string payload;
    assert(sub.read_frame(op, payload));
    assert(contains(payload, "real"));

    // And the reason the line is needed: publish() itself never checked, which
    // is what an unguarded custom route was really calling.
    hub.publish("hooks", "direct");
    assert(sub.read_frame(op, payload));
    assert(contains(payload, "direct"));

    sub.close();
    srv.stop();
    if (th.joinable())
        th.join();

    pass("ws_broadcast_hub: a shadowing custom route guards itself with origin_allowed()");
}

} // namespace

void run_ws_broadcast_hub_tests()
{
    test_hub_ingest_and_deliver();
    test_hub_topic_isolation();
    test_hub_wildcard_and_fanout();
    test_hub_replay_on_connect();
    test_hub_replay_bounded_by_bytes();
    test_hub_replay_keeps_full_depth_when_small();
    test_hub_total_order_is_identical_for_every_subscriber();
    test_hub_order_holds_under_concurrent_publishers();
    test_hub_unsubscribe_on_close();
    test_hub_direct_publish_api();
    test_hub_raw_framing();
    test_hub_backpressure_bounded();
    test_origin_policy_defaults_and_lookalikes();
    test_hub_rejects_foreign_origin_on_ws();
    test_hub_allows_loopback_and_listed_origins();
    test_hub_origin_check_opts_out();
    test_hub_ingest_rejects_foreign_origin();
    test_hub_custom_route_guards_itself();
}

#else // !SNICHOLLS_HAS_WS_BROADCAST_HUB

void run_ws_broadcast_hub_tests() {}

#endif
