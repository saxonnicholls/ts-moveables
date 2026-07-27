//
//  loop_dispatch.cpp
//  TSMoveables
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - what does typed dispatch actually cost?
//
//  The event loop's pitch is that its dispatch layer is `moveable_signal`:
//  weak-pointer lifetime tracking, guaranteed connection order, reentrancy
//  that is already proven, RAII teardown. FUTURE_DIRECTIONS §7 claimed that
//  costs nothing worth having at this altitude, because an `epoll_wait` or
//  `read` syscall is a microsecond and a signal emission is tens of
//  nanoseconds - and then owed a measurement.
//
//  This is that measurement. The same workload is driven twice against the
//  same descriptors in the same process:
//
//    raw    - epoll/kqueue called directly, a flat fd -> index table, and a
//             plain function call. No abstraction of any kind.
//    ours   - snicholls::event_loop: fd_watch handles, signal emission,
//             lifetime tracking, the dispatch tap.
//
//  The difference is the price of the abstraction. If it is small next to the
//  syscalls, the claim stands; if it is not, the claim was wrong and the
//  number says so. The two are interleaved and best-of-N so that a noisy
//  machine cannot flatter either one.
//
//      make bench-dispatch
//
//  Read the ratio, not the absolute rate - the absolute number is dominated
//  by this machine's syscall cost, which is exactly the point being made.
//

#include "../TSMoveables/event/loop.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if !SNICHOLLS_HAS_EVENT_LOOP

int main()
{
    std::printf("dispatch bench: POSIX only - skipped\n");
    return 0;
}

#else

#include <algorithm>
#include <chrono>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(__linux__)
#define BENCH_EPOLL 1
#include <sys/epoll.h>
#else
#define BENCH_KQUEUE 1
#include <sys/event.h>
#endif

using namespace snicholls;

namespace {

bool markdown = false;
volatile long long sink = 0;                    // keeps the handler from vanishing

double seconds_since(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

struct pairs {
    std::vector<int> a, b;
    explicit pairs(int n)
    {
        for (int i = 0; i < n; ++i) {
            int sv[2];
            if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
                std::exit(1);
            ::fcntl(sv[0], F_SETFL, O_NONBLOCK);
            ::fcntl(sv[1], F_SETFL, O_NONBLOCK);
            a.push_back(sv[0]);
            b.push_back(sv[1]);
        }
    }
    ~pairs()
    {
        for (int fd : a) ::close(fd);
        for (int fd : b) ::close(fd);
    }
};

void poke(const pairs& p)
{
    const char c = 'x';
    for (int fd : p.b)
        (void)!::write(fd, &c, 1);
}

// ------------------------------------------------------------------- raw
// A readiness loop with nothing between the poller and the work

double run_raw(const pairs& p, int rounds)
{
    const int n = int(p.a.size());
#if defined(BENCH_EPOLL)
    const int ep = ::epoll_create1(0);
    for (int i = 0; i < n; ++i) {
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = p.a[i];
        ::epoll_ctl(ep, EPOLL_CTL_ADD, p.a[i], &ev);
    }
    const std::size_t cap = std::size_t(n);
    std::vector<epoll_event> evs(cap);      // named size: (std::size_t(n)) is a function decl
#else
    const int kq = ::kqueue();
    for (int i = 0; i < n; ++i) {
        struct kevent ev;
        EV_SET(&ev, uintptr_t(p.a[i]), EVFILT_READ, EV_ADD, 0, 0, nullptr);
        ::kevent(kq, &ev, 1, nullptr, 0, nullptr);
    }
    const std::size_t cap = std::size_t(n);
    std::vector<struct kevent> evs(cap);    // named size: (std::size_t(n)) is a function decl
#endif

    const auto t0 = std::chrono::steady_clock::now();
    long long delivered = 0;
    for (int r = 0; r < rounds; ++r) {
        poke(p);
        int seen = 0;
        while (seen < n) {
            int k = 0;
#if defined(BENCH_EPOLL)
            k = ::epoll_wait(ep, evs.data(), n, 1000);
            for (int i = 0; i < k; ++i) {
                char c;
                (void)!::read(evs[i].data.fd, &c, 1);
                sink += evs[i].data.fd;         // the "handler"
            }
#else
            struct timespec ts { 1, 0 };
            k = ::kevent(kq, nullptr, 0, evs.data(), n, &ts);
            for (int i = 0; i < k; ++i) {
                char c;
                (void)!::read(int(evs[i].ident), &c, 1);
                sink += int(evs[i].ident);
            }
#endif
            if (k <= 0)
                break;
            seen += k;
            delivered += k;
        }
    }
    const double secs = seconds_since(t0);
#if defined(BENCH_EPOLL)
    ::close(ep);
#else
    ::close(kq);
#endif
    return secs * 1e9 / double(delivered ? delivered : 1);
}

// ------------------------------------------------------------------- ours
// The same work, through fd_watch handles and signal emission

double run_ours(const pairs& p, int rounds)
{
    const int n = int(p.a.size());
    event_loop loop;
    std::vector<event_loop::fd_watch> watches;
    watches.reserve(std::size_t(n));
    long long delivered = 0;

    for (int i = 0; i < n; ++i) {
        auto w = loop.watch(p.a[i], fd_interest::read);
        const int fd = p.a[i];
        w.on_readable([fd, &delivered] {
            char c;
            (void)!::read(fd, &c, 1);
            sink += fd;                         // the same "handler"
            ++delivered;
        });
        watches.push_back(std::move(w));
    }

    const auto t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < rounds; ++r) {
        poke(p);
        const long long want = delivered + n;
        while (delivered < want)
            if (!loop.run_once(std::chrono::milliseconds{1000}))
                break;
    }
    const double secs = seconds_since(t0);
    watches.clear();
    return secs * 1e9 / double(delivered ? delivered : 1);
}

} // namespace

int main(int argc, char** argv)
{
    int n = 64, rounds = 2000, reps = 5;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--markdown") == 0) markdown = true;
        else if (std::strcmp(argv[i], "--fds") == 0 && i + 1 < argc) n = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--rounds") == 0 && i + 1 < argc) rounds = std::atoi(argv[++i]);
    }

    pairs p(n);
    double best_raw = 1e30, best_ours = 1e30;
    // Interleaved and best-of, so a busy machine cannot favour whichever ran
    // while it happened to be quiet
    for (int i = 0; i < reps; ++i) {
        best_raw = std::min(best_raw, run_raw(p, rounds));
        best_ours = std::min(best_ours, run_ours(p, rounds));
    }

    const double overhead = best_ours - best_raw;
    const char* backend =
#if defined(BENCH_EPOLL)
        "epoll";
#else
        "kqueue";
#endif

    if (markdown) {
        std::printf("### `make bench-dispatch` - what typed dispatch costs\n\n"
                    "| Loop | Per event | Events/s |\n|---|---|---|\n");
        std::printf("| raw %s, direct call | %.0f ns | %.2f M/s |\n",
                    backend, best_raw, 1000.0 / best_raw);
        std::printf("| event_loop, signal dispatch | %.0f ns | %.2f M/s |\n",
                    best_ours, 1000.0 / best_ours);
        std::printf("| **abstraction cost** | **%.0f ns (%.0f%%)** | - |\n",
                    overhead, 100.0 * overhead / best_raw);
        std::printf("\n%d descriptors, %d rounds, best of %d interleaved. The absolute rate is "
                    "this machine's syscall cost; the ratio is the number that travels.\n",
                    n, rounds, reps);
    } else {
        std::printf("dispatch overhead - %d descriptors, %d rounds, best of %d interleaved\n\n",
                    n, rounds, reps);
        std::printf("  %-34s %7.0f ns/event   %6.2f M/s\n",
                    (std::string("raw ") + backend + ", direct call").c_str(),
                    best_raw, 1000.0 / best_raw);
        std::printf("  %-34s %7.0f ns/event   %6.2f M/s\n",
                    "event_loop, signal dispatch", best_ours, 1000.0 / best_ours);
        std::printf("\n  typed dispatch costs %.0f ns/event (%.0f%% on top of the syscalls).\n",
                    overhead, 100.0 * overhead / best_raw);
        std::printf("  Read the ratio, not the rate: the absolute number is this machine's\n"
                    "  syscall cost, which is the comparison being made.\n");
    }
    return 0;
}

#endif
