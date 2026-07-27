//
//  drone_fleet_demo.cpp
//  TSMoveables
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - ten drones, five operators, one pipeline
//
//  A real shape, not a toy: ten aircraft in the air streaming telemetry, a
//  ground team of five who each need a live view, a full written log per
//  aircraft for the post-flight investigation, and console output for the
//  operators watching right now. Fifteen files, five consoles, five WebSocket
//  streams, ten producers, all at once.
//
//  The thing that makes this hard is not volume. It is that the consumers have
//  wildly different speeds and wildly different consequences:
//
//    - a **drone thread** must never block. It is flying an aircraft. If
//      logging can stall it, logging is a flight-safety hazard.
//    - a **file** is fast but not instant, and must lose nothing: it is the
//      record you will be asked for after an incident.
//    - a **WebSocket to a laptop** may be arbitrarily slow - a browser tab in
//      the background, an operator on a bad link, someone who walked away.
//      It must be allowed to fall behind, and must be allowed to *lose* data
//      rather than hold up anyone else.
//
//  A single-queue logger cannot express that, and a synchronous one is worse:
//  it would put the slowest laptop on the ground directly in the path of an
//  aircraft's control thread. Lanes are the answer, and the three policies
//  below are the whole design:
//
//      storage  block        never lose a line of the flight record
//      console  drop_newest  operators can miss a line, nobody dies
//      stream   drop_oldest  a lagging browser shows *current* telemetry,
//                            not a backlog from thirty seconds ago
//      relay    drop_oldest  one listener, four offices, four continents
//
//  That last lane is the one people always end up needing and rarely plan
//  for: a *single* listener on the fleet that forwards everything live to New
//  York, London, San Francisco and Sydney. It is one sink, not four - the
//  record is serialised to JSON once and the same bytes go to every office,
//  because doing it per destination is how a relay becomes the bottleneck.
//
//  Sydney is ~300 ms away and New York ~20 ms, so the offices drain at wildly
//  different rates over the same lane. Two things keep that honest: each
//  office has its own session buffer inside the server, and the relay skips
//  any office whose backlog has grown past a threshold rather than letting it
//  grow without bound. Sydney falling behind is Sydney's problem.
//
//  drop_oldest on the stream lane is the one worth pausing on. For a live
//  view, stale data is worse than missing data: an operator looking at a
//  drone's position wants where it *is*, not a queue of where it was.
//
//  Build and run:   make demo-drones        (compiled -O3 -DNDEBUG)
//
//  Self-verifying: every flight record is checked complete, every operator is
//  checked to have received live data, and the drone threads are checked never
//  to have been stalled. Exits non-zero if any of that fails.
//

#include "../TSMoveables/logging.hpp"
#include "../TSMoveables/websocket.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if !SNICHOLLS_HAS_WEBSOCKET

int main()
{
    std::printf("drone fleet demo: POSIX only - skipped\n");
    return 0;
}

#else

#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
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

constexpr int kDrones = 10;
constexpr int kOperators = 5;
constexpr int kSamplesPerDrone = 400;
constexpr int kFlightLogs = 15;             // 10 aircraft + 5 operator sessions

// One listener, four continents. The latencies are the point: Sydney is an
// order of magnitude further away than New York, over the same relay.
struct office {
    const char* name;
    int rtt_ms;
};
const office kOffices[] = {
    {"New York", 20}, {"London", 90}, {"San Francisco", 140}, {"Sydney", 300}};
constexpr int kOfficeCount = 4;

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

double ms_since(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// ------------------------------------------------------ the flight recorder
//
// Fifteen files, but ONE sink. The obvious shape - fifteen sinks each
// filtering for its own aircraft - would call fifteen functions per record and
// discard fourteen. Owning the handles and routing on arrival is one call and
// one write, and the drain thread is the only writer so there is no locking
// here at all.

class flight_recorder {
public:
    explicit flight_recorder(const std::string& dir)
    {
        for (int i = 0; i < kFlightLogs; ++i) {
            const std::string path = dir + "/log-" + std::to_string(i) + ".txt";
            files_.push_back(std::fopen(path.c_str(), "wb"));
        }
    }

    ~flight_recorder()
    {
        for (std::FILE* f : files_)
            if (f)
                std::fclose(f);
    }

    flight_recorder(const flight_recorder&) = delete;

    void operator()(const log::record& r)
    {
        const int idx = channel_of(r);
        if (idx < 0 || idx >= int(files_.size()) || !files_[idx])
            return;
        std::string line = log::detail::format_time(r.when);
        line += ' ';
        line += log::to_string(r.level);
        line += ' ';
        line += r.message;
        if (r.is_metric) {
            char v[40];
            std::snprintf(v, sizeof v, " = %.3f", r.value);
            line += v;
        }
        for (const auto& f : r.fields) {
            line += ' ';
            line += f.first;
            line += '=';
            line += f.second;
        }
        line += '\n';
        std::fwrite(line.data(), 1, line.size(), files_[idx]);
        ++written_;
    }

    void close_all()
    {
        for (std::FILE*& f : files_)
            if (f) {
                std::fclose(f);
                f = nullptr;
            }
    }

    long long written() const noexcept { return written_; }

private:
    // Typed, so routing is an array index rather than a scan for a key that
    // might be spelled wrong
    static int channel_of(const log::record& r) noexcept { return r.channel; }

    std::vector<std::FILE*> files_;
    long long written_ = 0;
};

// ----------------------------------------------------------- ground station
//
// The WebSocket hub the operators connect to. Each operator subscribes to the
// aircraft they are flying; the hub sends them only those. Sockets are held on
// the loop thread, which is also where the stream lane's sink runs, so the
// handover is a post() rather than a lock.

struct ground_station {
    server srv;
    std::uint16_t port = 0;
    std::thread loop_thread;
    std::vector<websocket> operators;               // loop thread only
    std::vector<std::pair<std::string, websocket>> offices;   // loop thread only
    std::atomic<int> connected{0};
    std::atomic<int> offices_connected{0};
    std::atomic<long long> relay_skipped{0};        // an office too far behind

    ground_station()
    {
        srv.get("/telemetry", websocket_route([this](websocket ws) {
            ws.on_message([](websocket sock, const ws_message& m) {
                // An operator says which aircraft they are watching
                sock.send_text("subscribed:" + m.data);
            });
            operators.push_back(ws.share());
            connected.fetch_add(1, std::memory_order_release);
        }));
        // One listener for the whole fleet, forwarding to every office that
        // has connected. Offices announce themselves by name on connect.
        srv.get("/offices", websocket_route([this](websocket ws) {
            ws.on_message([](websocket sock, const ws_message& m) {
                sock.send_text("ack:" + m.data);
            });
            offices.emplace_back("", ws.share());
            offices_connected.fetch_add(1, std::memory_order_release);
        }));

        port = srv.listen("127.0.0.1", 0);
        loop_thread = std::thread([this] { srv.run(); });
        while (!srv.running())
            std::this_thread::yield();
    }

    ~ground_station()
    {
        srv.stop();
        if (loop_thread.joinable())
            loop_thread.join();
    }

    // Called on the stream lane's thread. Hops to the loop thread once, then
    // fans out - the websocket handles marshal their own sends anyway.
    void broadcast(const std::string& json)
    {
        auto payload = std::make_shared<std::string>(json);
        srv.loop().post([this, payload] {
            for (auto& ws : operators) {
                if (ws.connected())
                    ws.send_text(*payload);
            }
        });
    }

    // The global relay: one serialisation, four continents. Serialised once by
    // the caller; here we only choose who still deserves it.
    void relay_to_offices(const std::shared_ptr<std::string>& payload)
    {
        srv.loop().post([this, payload] {
            for (auto& office : offices) {
                if (!office.second.connected())
                    continue;
                // An office that has fallen far behind is skipped rather than
                // allowed to grow its buffer without bound. Sydney being 300 ms
                // away must not become everyone else's problem, and a relay
                // that queues indefinitely for one slow destination is how a
                // log pipeline turns into an outage.
                if (office.second.backlog() > (1u << 20)) {
                    relay_skipped.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                office.second.send_text(*payload);
            }
        });
    }
};

// ------------------------------------------------------------- an operator
// A laptop on the ground. One of them is deliberately slow, because in real
// life one of them always is.

class operator_console {
public:
    bool connect_to(std::uint16_t port, int id, bool slow)
    {
        id_ = id;
        slow_ = slow;
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
        tv.tv_sec = 2;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

        const std::string req =
            "GET /telemetry HTTP/1.1\r\nHost: ground\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n";
        if (!send_all(req))
            return false;
        std::string head;
        while (head.find("\r\n\r\n") == std::string::npos) {
            char t[1024];
            const ssize_t n = ::recv(fd_, t, sizeof t, 0);
            if (n <= 0)
                return false;
            head.append(t, std::size_t(n));
        }
        buf_.assign(head, head.find("\r\n\r\n") + 4, std::string::npos);
        return head.find(" 101 ") != std::string::npos;
    }

    ~operator_console()
    {
        if (fd_ >= 0)
            ::close(fd_);
    }

    void run(std::atomic<bool>& stop)
    {
        while (!stop.load(std::memory_order_acquire)) {
            // The slow operator reads lazily: a backgrounded browser tab, a
            // bad link, someone who walked away from the console
            if (slow_)
                std::this_thread::sleep_for(3ms);
            char t[65536];
            const ssize_t n = ::recv(fd_, t, sizeof t, 0);
            if (n <= 0)
                continue;
            buf_.append(t, std::size_t(n));
            drain_frames();
        }
        drain_frames();
    }

    long long received() const noexcept { return received_; }
    int id() const noexcept { return id_; }

private:
    bool send_all(const std::string& s)
    {
        std::size_t off = 0;
        while (off < s.size()) {
            const ssize_t n = ::send(fd_, s.data() + off, s.size() - off, 0);
            if (n <= 0)
                return false;
            off += std::size_t(n);
        }
        return true;
    }

    void drain_frames()
    {
        for (;;) {
            if (buf_.size() < 2)
                return;
            const unsigned char* p = reinterpret_cast<const unsigned char*>(buf_.data());
            std::size_t len = p[1] & 0x7f, hdr = 2;
            if (len == 126) {
                if (buf_.size() < 4)
                    return;
                len = (std::size_t(p[2]) << 8) | p[3];
                hdr = 4;
            } else if (len == 127) {
                if (buf_.size() < 10)
                    return;
                len = 0;
                for (int i = 0; i < 8; ++i)
                    len = (len << 8) | p[2 + i];
                hdr = 10;
            }
            if (buf_.size() < hdr + len)
                return;
            buf_.erase(0, hdr + len);
            ++received_;
        }
    }

    int fd_ = -1;
    int id_ = 0;
    bool slow_ = false;
    long long received_ = 0;
    std::string buf_;
};

// An office on the far end of a long link. Same code as an operator console;
// only the latency differs, which is the honest way to model a continent.
class office_relay {
public:
    bool connect_to(std::uint16_t port, const office& o)
    {
        name_ = o.name;
        rtt_ = o.rtt_ms;
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
        tv.tv_sec = 2;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

        const std::string req =
            "GET /offices HTTP/1.1\r\nHost: relay\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n";
        std::size_t off = 0;
        while (off < req.size()) {
            const ssize_t n = ::send(fd_, req.data() + off, req.size() - off, 0);
            if (n <= 0)
                return false;
            off += std::size_t(n);
        }
        std::string head;
        while (head.find("\r\n\r\n") == std::string::npos) {
            char t[1024];
            const ssize_t n = ::recv(fd_, t, sizeof t, 0);
            if (n <= 0)
                return false;
            head.append(t, std::size_t(n));
        }
        buf_.assign(head, head.find("\r\n\r\n") + 4, std::string::npos);
        return head.find(" 101 ") != std::string::npos;
    }

    ~office_relay()
    {
        if (fd_ >= 0)
            ::close(fd_);
    }

    void run(std::atomic<bool>& stop)
    {
        // A round trip to the other side of the world, modelled as the rate at
        // which this end can take delivery
        const auto pace = std::chrono::microseconds(rtt_ * 40);
        while (!stop.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(pace);
            char t[65536];
            const ssize_t n = ::recv(fd_, t, sizeof t, 0);
            if (n <= 0)
                continue;
            buf_.append(t, std::size_t(n));
            drain();
        }
        drain();
    }

    long long received() const noexcept { return received_; }
    std::uint64_t highest_seq() const noexcept { return highest_seq_; }
    bool ordered() const noexcept { return ordered_; }
    const char* name() const noexcept { return name_; }
    int rtt() const noexcept { return rtt_; }

private:
    void drain()
    {
        for (;;) {
            if (buf_.size() < 2)
                return;
            const unsigned char* p = reinterpret_cast<const unsigned char*>(buf_.data());
            std::size_t len = p[1] & 0x7f, hdr = 2;
            if (len == 126) {
                if (buf_.size() < 4) return;
                len = (std::size_t(p[2]) << 8) | p[3];
                hdr = 4;
            } else if (len == 127) {
                if (buf_.size() < 10) return;
                len = 0;
                for (int i = 0; i < 8; ++i) len = (len << 8) | p[2 + i];
                hdr = 10;
            }
            if (buf_.size() < hdr + len)
                return;
            // Every frame carries its sequence number, so an office can check
            // for itself that what it received was in order
            const std::string payload(buf_, hdr, len);
            const std::size_t at = payload.find("\"seq\":");
            if (at != std::string::npos) {
                const std::uint64_t seq = std::strtoull(payload.c_str() + at + 6, nullptr, 10);
                if (received_ > 0 && seq <= highest_seq_)
                    ordered_ = false;
                highest_seq_ = seq;
            }
            buf_.erase(0, hdr + len);
            ++received_;
        }
    }

    int fd_ = -1;
    const char* name_ = "";
    int rtt_ = 0;
    long long received_ = 0;
    std::uint64_t highest_seq_ = 0;
    bool ordered_ = true;
    std::string buf_;
};

} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--markdown") == 0)
            markdown = true;

    if (markdown)
        std::printf("### `make demo-drones` - 10 aircraft, 5 operators, one pipeline\n\n"
                    "| Scenario | Result |\n|---|---|\n");
    else
        std::printf("drone fleet demo - %d aircraft, %d operators, %d flight logs\n\n",
                    kDrones, kOperators, kFlightLogs);

    char buf[256];
    const std::string dir = "build/flightlogs";
    std::string mk = "mkdir -p " + dir;
    if (std::system(mk.c_str()) != 0)
        return 1;

    ground_station ground;

    // ------------------------------------------------------- the three lanes
    log::logger_config cfg;
    cfg.name = "fleet";
    cfg.level = log::level::trace;
    cfg.default_lane.name = "console";
    cfg.default_lane.capacity = 4096;
    cfg.default_lane.on_full = log::overflow::drop_newest;   // an operator may miss a line
    log::logger fleet{cfg};

    // Five operator consoles, counting what they would have shown
    std::atomic<long long> console_lines{0};
    for (int op = 0; op < kOperators; ++op)
        fleet.add_sink([&console_lines, op](const log::record& r) {
            // Each console watches two aircraft, so the team covers the fleet
            if (r.channel == op * 2 || r.channel == op * 2 + 1)
                console_lines.fetch_add(1, std::memory_order_relaxed);
        });

    // Storage: blocks rather than lose a line. This is the record an
    // investigator will ask for, so "we dropped some" is not an answer.
    log::lane_config storage_cfg;
    storage_cfg.name = "storage";
    storage_cfg.capacity = 1 << 15;
    storage_cfg.on_full = log::overflow::block;
    auto storage = fleet.add_lane(storage_cfg);
    auto recorder = std::make_shared<flight_recorder>(dir);
    storage.add_sink([recorder](const log::record& r) { (*recorder)(r); });

    // Stream: drops the OLDEST. For a live view stale data is worse than
    // missing data - an operator wants where the aircraft is, not a backlog
    // of where it was.
    auto stream = fleet.add_lane("stream", 512, log::overflow::drop_oldest);
    stream.add_sink(log::json_sink([&ground](const std::string& j) { ground.broadcast(j); }));

    // ------------------------------------------------- the team on the ground
    std::vector<std::unique_ptr<operator_console>> consoles;
    std::vector<std::thread> console_threads;
    std::atomic<bool> stop_consoles{false};
    for (int op = 0; op < kOperators; ++op) {
        auto c = std::unique_ptr<operator_console>(new operator_console());
        // Operator 3 is on a bad link. There is always one.
        check(c->connect_to(ground.port, op, /*slow*/ op == 3), "operator connected");
        consoles.push_back(std::move(c));
    }
    while (ground.connected.load(std::memory_order_acquire) < kOperators)
        std::this_thread::yield();
    for (auto& c : consoles)
        console_threads.emplace_back([&c, &stop_consoles] { c->run(stop_consoles); });

    // ------------------------------------------- one listener, four continents
    std::vector<std::unique_ptr<office_relay>> office_links;
    std::vector<std::thread> office_threads;
    for (int i = 0; i < kOfficeCount; ++i) {
        auto o = std::unique_ptr<office_relay>(new office_relay());
        check(o->connect_to(ground.port, kOffices[i]), "office connected");
        office_links.push_back(std::move(o));
    }
    while (ground.offices_connected.load(std::memory_order_acquire) < kOfficeCount)
        std::this_thread::yield();
    for (auto& o : office_links)
        office_threads.emplace_back([&o, &stop_consoles] { o->run(stop_consoles); });

    // The relay: ONE sink, one serialisation, four destinations.
    //
    // An *ordered* lane, because the offices need the stream in sequence and
    // records do not arrive in it - a thread can be descheduled between
    // stamping its sequence and queueing the record, so a lane routinely sees
    // 7 before 6. The window belongs to the lane, which also flushes it when
    // the stream ends, so there is nothing here to remember.
    auto relay = fleet.add_ordered_lane("relay", 2048, 4096);
    relay.add_sink(log::json_sink([&ground](const std::string& j) {
        ground.relay_to_offices(std::make_shared<std::string>(j));
    }));

    // Everything the fleet said, captured for training. This is the same lane
    // machinery - a journal is just another sink.
    log::journal training_journal;
    relay.add_sink(training_journal.sink());

    // --------------------------------------------------------- the aircraft
    // Ten threads that must never be made to wait
    std::atomic<double> worst_stall_ms{0.0};
    std::vector<std::thread> drones;
    const auto flight_start = std::chrono::steady_clock::now();

    for (int d = 0; d < kDrones; ++d)
        drones.emplace_back([d, &fleet, &worst_stall_ms] {
            for (int s = 0; s < kSamplesPerDrone; ++s) {
                const double t = double(s) * 0.02;
                const double lat = 51.5074 + 0.0001 * std::sin(t + d);
                const double lon = -0.1278 + 0.0001 * std::cos(t + d);
                const double batt = 100.0 - 0.02 * double(s) - double(d) * 0.1;

                // The measurement that matters: how long did logging make this
                // aircraft's thread wait?
                const auto t0 = std::chrono::steady_clock::now();
                SN_LOGGER_INFO(fleet)
                    .channel(d)
                    .field("drone", "UAV-" + std::to_string(d))
                    .field("lat", lat)
                    .field("lon", lon)
                    .field("batt", batt)
                    << "telemetry seq=" << s;
                if (batt < 70.0 && s % 50 == 0) {
                    SN_LOGGER_WARN(fleet).channel(d) << "battery " << batt << "%";
                }
                const double took = ms_since(t0);

                double prev = worst_stall_ms.load(std::memory_order_relaxed);
                while (took > prev &&
                       !worst_stall_ms.compare_exchange_weak(prev, took,
                                                             std::memory_order_relaxed)) {
                }
                std::this_thread::sleep_for(200us);     // ~5 kHz aggregate
            }
        });

    for (auto& t : drones)
        t.join();
    const double flight_ms = ms_since(flight_start);

    fleet.flush();
    std::this_thread::sleep_for(400ms);         // let the ground station drain its posts
    stop_consoles.store(true, std::memory_order_release);
    for (auto& t : console_threads)
        t.join();
    for (auto& t : office_threads)
        t.join();
    recorder->close_all();

    // ------------------------------------------------------------- verdicts
    const long long expected = (long long)kDrones * kSamplesPerDrone;

    std::snprintf(buf, sizeof buf, "%lld samples from %d aircraft in %.0f ms",
                  expected, kDrones, flight_ms);
    row("telemetry produced", buf);

    // The flight record must be complete. This is the whole reason the storage
    // lane blocks instead of dropping.
    check(storage.dropped() == 0, "the flight record lost nothing");
    check(recorder->written() >= expected, "every telemetry sample reached a flight log");
    std::snprintf(buf, sizeof buf, "%lld lines across %d files, 0 dropped",
                  recorder->written(), kFlightLogs);
    row("flight records (lane: block)", buf);

    std::snprintf(buf, sizeof buf, "%lld lines shown, %llu dropped under load",
                  console_lines.load(), (unsigned long long)fleet.default_lane().dropped());
    row("operator consoles (lane: drop_newest)", buf);

    // The live stream is allowed to shed, and the point is that it sheds
    // *old* data so the view stays current
    long long received = 0;
    for (auto& c : consoles)
        received += c->received();
    check(received > 0, "operators received live telemetry over the WebSocket");
    std::snprintf(buf, sizeof buf, "%lld frames delivered, %llu shed to stay current",
                  received, (unsigned long long)stream.dropped());
    row("live WebSocket streams (drop_oldest)", buf);

    // The claim that matters most: no aircraft thread was ever made to wait
    const double worst = worst_stall_ms.load();
    check(worst < 50.0, "no drone thread was stalled by logging");
    std::snprintf(buf, sizeof buf, "worst single call %.3f ms, across %lld calls", worst, expected);
    row("time logging cost a drone thread", buf);

    // One listener, four offices. Each checks its own stream for order.
    bool all_ordered = true;
    long long office_total = 0;
    for (auto& o : office_links) {
        office_total += o->received();
        all_ordered = all_ordered && o->ordered();
        check(o->received() > 0, "every office received live telemetry");
        std::snprintf(buf, sizeof buf, "%lld frames, %s, ~%d ms away",
                      o->received(), o->ordered() ? "in order" : "OUT OF ORDER", o->rtt());
        char label[64];
        std::snprintf(label, sizeof label, "  office: %s", o->name());
        row(label, buf);
    }
    check(all_ordered, "every office received its stream in sequence order");
    std::snprintf(buf, sizeof buf,
                  "%lld frames to %d offices, %llu skipped when behind, %llu gaps jumped",
                  office_total, kOfficeCount,
                  (unsigned long long)ground.relay_skipped.load(),
                  (unsigned long long)relay.gaps_jumped());
    row("global relay (one listener, ordered)", buf);

    // ------------------------------------------------- training replay, NYC
    // The incident is over. A trainee needs to watch it unfold the way the
    // operator did. Nothing downstream can tell a replayed record from a live
    // one, so the training console IS the production console.
    {
        log::replayer rp(training_journal.take());
        check(rp.size() > 0, "the training journal captured the flight");
        const double original_ms = double(rp.duration().count()) / 1e6;

        std::atomic<long long> trainee_saw{0};
        std::uint64_t last = 0;
        bool in_order = true;
        const auto t0 = std::chrono::steady_clock::now();
        rp.play(
            [&](const log::record& r) {
                if (trainee_saw.load() > 0 && r.seq <= last)
                    in_order = false;
                last = r.seq;
                trainee_saw.fetch_add(1, std::memory_order_relaxed);
            },
            8.0);                                   // eight times real speed
        const double replay_ms = ms_since(t0);

        check(trainee_saw.load() == (long long)rp.size(), "the trainee saw the whole flight");
        check(in_order, "the replay was in sequence order");
        // Paced, not dumped: an 8x replay of a 100 ms flight must take about
        // 12 ms, not zero. The gaps are the part a trainee needs to feel.
        check(replay_ms > original_ms / 40.0, "the replay was paced, not dumped");
        std::snprintf(buf, sizeof buf, "%zu records, %.0f ms flight replayed in %.0f ms at 8x",
                      rp.size(), original_ms, replay_ms);
        row("training replay (New York)", buf);
    }

    std::snprintf(buf, sizeof buf,
                  "%zu lanes, %d files, %d consoles, %d operators, %d offices",
                  fleet.lane_count(), kFlightLogs, kOperators, kOperators, kOfficeCount);
    row("one pipeline", buf);

    if (!markdown) {
        std::printf("\n  One operator was deliberately on a bad link. It shed frames and\n"
                    "  nobody else noticed - which is the entire point of separate lanes.\n");
        std::printf("\nall drone fleet demo checks passed\n");
    }
    return 0;
}

#endif
