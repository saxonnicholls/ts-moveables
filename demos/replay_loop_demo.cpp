//
//  replay_loop_demo.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - a deterministic, replayable event loop
//
//  This is the bar FUTURE_DIRECTIONS §7 set for the event loop and did not
//  clear until now: journal a scripted session, replay it into a *fresh*
//  handler graph, and assert the output reproduces bit for bit.
//
//  Why an event loop can do this at all is the whole argument for the design.
//  Every delivery - fd readiness, timer fire, posted task - is announced on
//  the loop's `on_dispatch` tap before it happens. A loop whose dispatch is
//  hidden inside a callback table has nothing to record; one whose dispatch is
//  a typed signal has a seam. Record what crossed that seam and you can drive
//  the same handler graph again later with no sockets, no timers, no threads
//  and no clock, and get the same answer.
//
//  That matters because event-driven bugs are the ones that do not reproduce.
//  "It happened once on Tuesday under load" is not a bug report you can act
//  on; a journal that replays it deterministically is.
//
//  Two halves:
//    1. the loop itself - fds, timers and cross-thread posts, replayed
//    2. the HTTP server on top of it, where the same discipline replays a
//       whole session of requests through a fresh router
//
//  Build and run:   make demo-replay        (compiled -O3 -DNDEBUG)
//
//  Self-verifying: it exits non-zero if a replay diverges, so CI runs it as an
//  integration test on every push.
//

#include "../TSMoveables/event/loop.hpp"
#include "../TSMoveables/http/server.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if !SNICHOLLS_HAS_EVENT_LOOP

int main()
{
    std::printf("replay demo: POSIX only - skipped\n");
    return 0;
}

#else

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace snicholls;
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
        std::printf("  %-44s %s\n", what, result);
}

// FNV-1a: the point is that two runs agree, not cryptographic strength
struct hasher {
    std::uint64_t h = 1469598103934665603ull;
    void feed(const void* p, std::size_t n)
    {
        const unsigned char* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    }
    void feed(const std::string& s) { feed(s.data(), s.size()); }
};

// ------------------------------------------------------- the journal format
//
// One record per delivery. The tap supplies what kind of event it was and
// which fd or timer it came from; the handler supplies the payload it acted
// on. Together that is everything the graph saw.

struct record {
    dispatch_info::kind what;
    int fd;
    std::uint64_t timer_id;
    std::string payload;
};

// --------------------------------------------------- the graph under test
//
// Deliberately stateful and order-sensitive: a replay that got the ordering
// wrong, dropped an event or duplicated one would produce a different digest.
// It knows nothing about sockets - it consumes events, which is what makes it
// replayable.

class graph {
public:
    void on_data(int fd, const std::string& bytes)
    {
        ++events_;
        digest_.feed("D", 1);
        digest_.feed(&fd, sizeof fd);
        digest_.feed(bytes);
        // Order-sensitive state: a reordered replay diverges immediately
        running_ = running_ * 31 + bytes.size() + std::size_t(fd);
        digest_.feed(&running_, sizeof running_);
    }

    void on_timer(std::uint64_t id)
    {
        ++events_;
        digest_.feed("T", 1);
        digest_.feed(&id, sizeof id);
        running_ ^= (id * 2654435761ull);
        digest_.feed(&running_, sizeof running_);
    }

    void on_task(const std::string& label)
    {
        ++events_;
        digest_.feed("K", 1);
        digest_.feed(label);
        running_ += label.size();
        digest_.feed(&running_, sizeof running_);
    }

    std::uint64_t digest() const noexcept { return digest_.h; }
    long long events() const noexcept { return events_; }

private:
    hasher digest_;
    std::size_t running_ = 0;
    long long events_ = 0;
};

struct socket_pair {
    int a = -1, b = -1;
    socket_pair()
    {
        int sv[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
            std::exit(1);
        a = sv[0];
        b = sv[1];
        ::fcntl(a, F_SETFL, O_NONBLOCK);
        ::fcntl(b, F_SETFL, O_NONBLOCK);
    }
    ~socket_pair()
    {
        if (a >= 0) ::close(a);
        if (b >= 0) ::close(b);
    }
};

} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--markdown") == 0)
            markdown = true;

    if (markdown)
        std::printf("### `make demo-replay` - deterministic event-loop replay\n\n"
                    "| Scenario | Result |\n|---|---|\n");
    else
        std::printf("replay demo - journal a live session, replay it, compare digests\n\n");

    char buf[192];
    std::vector<record> journal;
    std::uint64_t live_digest = 0;
    long long live_events = 0;

    // ================================================== 1. record a live session
    {
        event_loop loop;
        graph g;

        socket_pair s1, s2, s3;
        const int fds[3] = {s1.a, s2.a, s3.a};
        const int peers[3] = {s1.b, s2.b, s3.b};

        // The tap sees every delivery before it happens. Payloads are captured
        // by the handlers, which is the only part the tap cannot know.
        std::string pending_task_label;
        auto tap = loop.on_dispatch().connect([&](const dispatch_info& d) {
            journal.push_back(record{d.what, d.fd, d.timer_id, {}});
        });

        std::vector<event_loop::fd_watch> watches;
        for (int i = 0; i < 3; ++i) {
            auto w = loop.watch(fds[i], fd_interest::read);
            const int fd = fds[i];
            w.on_readable([&g, &journal, fd] {
                char tmp[512];
                const ssize_t n = ::read(fd, tmp, sizeof tmp);
                if (n <= 0)
                    return;
                const std::string bytes(tmp, std::size_t(n));
                // The tap already recorded the *event*; the handler records
                // the bytes it acted on, which the loop could not have known
                if (!journal.empty())
                    journal.back().payload = bytes;
                g.on_data(fd, bytes);
            });
            watches.push_back(std::move(w));
        }

        auto t_fast = loop.every(2ms);
        t_fast.on_fire([&g, &t_fast] { g.on_timer(t_fast.id()); });
        auto t_once = loop.after(6ms);
        t_once.on_fire([&g, &t_once] { g.on_timer(t_once.id()); });

        // A scripted session: writes from a foreign thread, posted tasks, and
        // timers all interleaving as they would in a real program
        std::thread writer([&] {
            for (int round = 0; round < 12; ++round) {
                const std::string msg = "round-" + std::to_string(round) + ";";
                [[maybe_unused]] ssize_t w = ::write(peers[round % 3], msg.data(), msg.size());
                loop.post([&g, &journal, round] {
                    const std::string label = "task-" + std::to_string(round);
                    if (!journal.empty())
                        journal.back().payload = label;
                    g.on_task(label);
                });
                std::this_thread::sleep_for(1ms);
            }
        });

        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (g.events() < 30 && std::chrono::steady_clock::now() < deadline)
            loop.run_once(20ms);
        writer.join();
        for (int i = 0; i < 10; ++i)
            loop.run_once(5ms);

        watches.clear();
        live_digest = g.digest();
        live_events = g.events();

        check(live_events >= 30, "the live session produced enough events");
        std::snprintf(buf, sizeof buf, "%lld events (%zu journalled), digest %016llx",
                      live_events, journal.size(), (unsigned long long)live_digest);
        row("live session over 3 sockets + 2 timers", buf);
    }

    // ============================================ 2. replay into a fresh graph
    // No loop, no sockets, no timers, no threads, no clock. Just the journal
    // driven into a graph that has never seen any of it.
    {
        graph g;
        for (const auto& r : journal) {
            switch (r.what) {
            case dispatch_info::kind::readable:
                if (!r.payload.empty())
                    g.on_data(r.fd, r.payload);
                break;
            case dispatch_info::kind::timer:
                g.on_timer(r.timer_id);
                break;
            case dispatch_info::kind::task:
                if (!r.payload.empty())
                    g.on_task(r.payload);
                break;
            default:
                break;
            }
        }

        check(g.events() == live_events, "replay delivered exactly as many events");
        check(g.digest() == live_digest, "replay digest matches the live run bit for bit");
        std::snprintf(buf, sizeof buf, "%lld events, digest %016llx - identical",
                      g.events(), (unsigned long long)g.digest());
        row("replayed with no sockets, timers or threads", buf);
    }

    // ================================ 3. a corrupted journal must NOT reproduce
    // A test that only ever passes proves nothing. Drop one event and the
    // digests must diverge - that is what makes the match above meaningful.
    {
        graph g;
        bool skipped = false;
        for (const auto& r : journal) {
            if (!skipped && r.what == dispatch_info::kind::readable && !r.payload.empty()) {
                skipped = true;             // lose exactly one delivery
                continue;
            }
            switch (r.what) {
            case dispatch_info::kind::readable:
                if (!r.payload.empty()) g.on_data(r.fd, r.payload);
                break;
            case dispatch_info::kind::timer: g.on_timer(r.timer_id); break;
            case dispatch_info::kind::task:
                if (!r.payload.empty()) g.on_task(r.payload);
                break;
            default: break;
            }
        }
        check(skipped, "the negative control actually dropped an event");
        check(g.digest() != live_digest, "a journal missing one event must NOT reproduce");
        row("negative control: one event dropped", "digest diverges, as it must");
    }

#if SNICHOLLS_HAS_HTTP_SERVER
    // ============================== 4. the same discipline, one layer up: HTTP
    // Because the server's routing and serialisation are just as free of IO as
    // the graph above, a journal of request bytes replays a whole session
    // through a fresh router and produces the same responses.
    {
        using namespace snicholls::http;

        std::vector<std::string> requests;
        for (int i = 0; i < 40; ++i)
            requests.push_back("GET /users/" + std::to_string(i) +
                               "/posts?page=" + std::to_string(i % 7) +
                               " HTTP/1.1\r\nHost: replay\r\nX-Seq: " + std::to_string(i) +
                               "\r\n\r\n");

        // A host with no IO behind it - responses land in a string
        struct capture_host final : connection_host {
            void deliver(request&, std::uint64_t) override {}
            void write_app(const char* d, std::size_t n) override { out.append(d, n); }
            void protocol_failure(int, const char*) override {}
            const server_config& config() const noexcept override { return cfg; }
            const char* http_date() override { return "Mon, 27 Jul 2026 00:00:00 GMT"; }
            bool live() const noexcept override { return true; }
            void switch_protocol(std::unique_ptr<protocol_delegate>) override {}
            server_config cfg;
            std::string out;
        };

        auto run_session = [&](const std::vector<std::string>& script) {
            router routes;
            routes.add(method::get, "/users/:uid/posts", [](const request&, responder) {});
            capture_host host;
            hasher h;
            for (const auto& wire : script) {
                request_parser p;
                std::size_t used = 0;
                if (p.parse(wire.data(), wire.size(), used) !=
                    request_parser::status::have_request)
                    continue;
                std::unordered_map<std::string, std::string> params;
                bool exists = false;
                std::string allowed;
                const bool matched = routes.match(p.message(), params, exists, allowed) != nullptr;
                response res(matched ? 200 : 404);
                res.content("uid=" + params["uid"] + ";page=" +
                            p.message().query_param("page"), "text/plain");
                std::string out;
                http1_protocol::serialise(res, false, true, host, out);
                h.feed(out);
            }
            return h.h;
        };

        const std::uint64_t live = run_session(requests);
        const std::uint64_t replayed = run_session(requests);      // fresh router, same journal
        auto shuffled = requests;
        std::swap(shuffled[3], shuffled[9]);
        const std::uint64_t reordered = run_session(shuffled);

        check(live == replayed, "an HTTP session replays to an identical response stream");
        check(live != reordered, "a reordered session must NOT reproduce");
        std::snprintf(buf, sizeof buf, "%zu requests, digest %016llx - identical, and order matters",
                      requests.size(), (unsigned long long)live);
        row("HTTP session replayed through a fresh router", buf);
    }
#endif

    if (!markdown)
        std::printf("\nall replay demo checks passed\n");
    return 0;
}

#endif
