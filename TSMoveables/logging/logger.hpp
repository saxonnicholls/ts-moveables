//
//  logging.hpp
//  TSMoveables
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - logging and telemetry, from any thread, anywhere
//
//      SN_LOG_INFO() << "order " << id << " filled at " << px;
//      SN_LOG_WARN().field("venue", "XLON") << "reject: " << why;
//
//  Call it from a method, a free function, a signal handler's worker, a
//  destructor, a thread you did not start. No locks to take, no logger to
//  thread through your call graph, no sink to keep alive by hand.
//
//  The architecture is one decision, and it is the one that matters.
//
//  moveable_signal runs its slots *on the emitting thread*. A logger that
//  emitted straight from LOG() would run every sink there too - so a
//  WebSocket write to a machine across the room, or a Postgres round trip,
//  would block whatever thread happened to log. That is worse than no logging
//  library at all, because it couples your latency to your slowest observer.
//
//  So LOG() does not emit. It stamps a record and pushes it into a bounded
//  lock-free mpmc_queue, and returns. One drain thread pops and emits the
//  signal. Two things fall out:
//
//    - **Producers never block.** The push is a CAS on a slot; if the queue is
//      full the record is dropped and counted, and dropping is reported rather
//      than hidden. The alternative - blocking the caller - means a slow sink
//      can stall a trading thread, which is not a trade anyone should make by
//      accident. `overflow::block` is available if you want it, named so that
//      choosing it is deliberate.
//    - **Sinks are called on one thread, in order.** A sink author needs no
//      locks and no thread-safety story of their own, which is most of why
//      writing a sink is three lines here.
//
//  And because one drain thread would mean a slow sink holding up a fast one,
//  sinks live in **lanes**. A lane is a queue and a thread of its own, so a
//  Postgres insert that takes 20 ms cannot stall the console:
//
//      snicholls::log::logger lg;
//      lg.add_sink(console_sink());                       // default lane
//      lg.add_sink(file_sink("app.log"));                 // same lane, still fast
//
//      auto db = lg.add_lane("postgres");                 // its own thread + queue
//      db.add_sink(my_postgres_sink());
//      auto net = lg.add_lane("ws", 1024, overflow::drop_oldest);
//      net.add_sink(json_sink([&](const std::string& j) { ws.broadcast(j); }));
//
//  A record is built once and handed to every lane as a shared pointer, so
//  fanning out to five lanes costs five refcount bumps, not five copies. Each
//  lane keeps its own depth, drop count and overflow policy - a lane feeding a
//  dashboard can drop the oldest and stay current, while the audit lane
//  blocks rather than lose a line, and neither decision affects the other.
//
//  Sinks are just slots, so they are connected with the same connect-and-park
//  idiom as everything else and die with the logger:
//
//      lg.on_record(console_sink());                    // colour, to stderr
//      lg.on_record(file_sink("app.log"));
//      lg.on_record(json_sink([&](const std::string& line) { ws.broadcast(line); }));
//
//  That last one is the point: JSON out of the same pipeline, streamed live
//  over the WebSocket server in this library to a browser, a React component,
//  another machine - without the logging code knowing any of that exists.
//
//  Telemetry rides the same pipeline. A counter or gauge is a record with a
//  numeric value, so it reaches every sink you already have, and a metrics
//  sink is just another slot.
//
//  Honest positioning against [spdlog](https://github.com/gabime/spdlog):
//  spdlog is faster at formatting and far more mature, and it has a
//  battle-tested rotating-file story we do not. What this has instead is the
//  fabric - one pipeline, any thread, sinks that are ordinary signal slots
//  with lifetime tracking, and a live WebSocket sink that falls out of
//  composition rather than a plugin API. Use spdlog if you want the fastest
//  formatter; use this if you want the log to *go somewhere interesting*
//  without wiring.
//

#ifndef logging_hpp
#define logging_hpp

#include "../moveable/signal.hpp"
#include "../concurrent/mpmc_queue.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#define SN_LOG_ISATTY _isatty
#define SN_LOG_FILENO _fileno
#else
#include <unistd.h>
#define SN_LOG_ISATTY ::isatty
#define SN_LOG_FILENO ::fileno
#endif

namespace snicholls {
namespace log {

enum class level : int { trace = 0, debug, info, warn, error, critical, off };

inline const char* to_string(level l) noexcept
{
    switch (l) {
    case level::trace:    return "TRACE";
    case level::debug:    return "DEBUG";
    case level::info:     return "INFO";
    case level::warn:     return "WARN";
    case level::error:    return "ERROR";
    case level::critical: return "CRITICAL";
    default:              return "OFF";
    }
}

// What a sink receives. Everything the call site knew, plus when and where.
struct record {
    // A total order across every thread and every lane.
    //
    // A timestamp alone cannot order concurrent logging, for two reasons that
    // both bite in practice: two threads can read the same clock value (system
    // clocks are not guaranteed to advance between reads, and on some
    // platforms the resolution is coarser than the gap between two log calls),
    // and a thread can be descheduled between stamping and queueing, so the
    // order records reach a lane need not be the order they were created in.
    // A single atomic counter settles both - it is the number an investigator
    // sorts by, and gaps in it are how you prove nothing was lost.
    std::uint64_t seq = 0;
    log::level level = level::info;
    // Which stream this belongs to - an aircraft, a tenant, a session. Typed
    // and first-class because routing on it is the common case: a recorder
    // that has to scan `fields` for a "ch" key and atoi it pays a linear scan
    // per record and misroutes silently on a typo.
    int channel = -1;
    std::chrono::system_clock::time_point when{};
    std::thread::id thread{};
    const char* file = "";                  // a literal from the macro; never freed
    int line = 0;
    std::string logger;                     // channel name
    std::string message;
    std::vector<std::pair<std::string, std::string>> fields;   // structured extras
    double value = 0.0;                     // telemetry payload
    bool is_metric = false;
};

// What to do when the queue is full. Dropping is the default because the
// alternative is letting a slow sink stall a producer, and a logger that can
// stall the program it is observing is a liability. Drops are counted.
enum class overflow { drop_newest, drop_oldest, block };

// One lane's settings. Each lane chooses independently, which is the point:
// the audit lane can block rather than lose a line while the dashboard lane
// drops the oldest and stays current.
struct lane_config {
    std::string name = "default";
    std::size_t capacity = 8192;                    // records in flight
    log::overflow on_full = overflow::drop_newest;
    // Non-zero installs a reordering window in front of this lane's sinks, so
    // they see records in `seq` order. Records do NOT arrive in that order - a
    // thread can be descheduled between stamping its sequence and queueing -
    // so a lane feeding anything order-sensitive wants this. It belongs to the
    // lane rather than the caller because the lane already knows when the
    // stream ends, and can flush without anyone remembering to.
    std::size_t ordered_window = 0;
    // How long a drain thread sleeps when it finds nothing. Small enough to be
    // invisible, large enough not to burn a core on an idle program.
    std::chrono::microseconds idle_sleep{200};
};

struct logger_config {
    std::string name = "app";
    log::level level = level::info;
    lane_config default_lane{};                     // created up front, so add_sink() just works
    // Where sequence numbers come from. Empty means the in-process counter,
    // which restarts at zero with the process; see persistent_sequence for a
    // source that does not.
    std::function<std::uint64_t()> sequence{};
};

namespace detail {

// Nanosecond wall-clock, ISO-8601. The *displayed* precision is always nine
// digits; the precision actually available is whatever the platform's
// system_clock offers (a microsecond on macOS, a nanosecond on most Linux),
// so the trailing digits may be zeros. Printing nine either way keeps the
// column width fixed and keeps a lexicographic sort correct - and record::seq
// is the field to sort by when exactness matters.
inline std::string format_time(std::chrono::system_clock::time_point tp)
{
    const auto secs = std::chrono::time_point_cast<std::chrono::seconds>(tp);
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(tp - secs).count();
    const std::time_t t = std::chrono::system_clock::to_time_t(secs);
    std::tm tm{};
#if defined(_WIN32)
    ::gmtime_s(&tm, &t);
#else
    ::gmtime_r(&t, &tm);
#endif
    char buf[48];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%09lldZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, (long long)ns);
    return buf;
}

// The one counter every record is ordered by. Relaxed is right: the counter
// carries no data of its own, and modification order on a single atomic is
// total regardless of ordering, which is exactly and only what an ordering
// stamp needs.
inline std::uint64_t next_seq() noexcept
{
    static std::atomic<std::uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

// Just enough escaping for a JSON string - quotes, backslash, control chars
inline void json_escape(const std::string& in, std::string& out)
{
    for (char c : in) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char esc[8];
                std::snprintf(esc, sizeof esc, "\\u%04x", c);
                out += esc;
            } else {
                out += c;
            }
        }
    }
}

inline const char* basename(const char* path) noexcept
{
    const char* slash = std::strrchr(path, '/');
    return slash ? slash + 1 : path;
}

} // namespace detail

// ------------------------------------------------------------------ logger
//
// Moveable, like everything else here: the core is heap-stable, so a fully
// configured logger with its sinks attached is a value you can build in a
// factory and move into place while its drain thread keeps running.

// ------------------------------------------------------ persistent ordering
//
// The in-process counter restarts at zero when the process does, and a fleet
// that restarts a logger mid-flight would then emit sequence numbers it has
// already used. Everything downstream that trusts `seq` - dedup, ordering,
// gap detection, "what happened between 41000 and 41500" - quietly breaks,
// and it breaks in a way nobody notices until the incident review.
//
// This is a sequence source that survives restarts, and the design is the one
// databases use for their id generators: **reserve a block, hand out from
// memory, persist only the block boundary.**
//
//     persistent_sequence seq{"/var/lib/fleet/seq"};
//     logger_config cfg;
//     cfg.sequence = seq.source();
//
// Cost per record is a relaxed fetch_add and a predictable branch; the file is
// touched once per block (10,000 records by default), not once per record. A
// crash loses the unused remainder of the current block, which is exactly the
// right trade: sequence numbers are skipped, never reused. A gap means "we
// restarted", which is information; a repeat means "your ordering is a lie".
class persistent_sequence {
public:
    explicit persistent_sequence(std::string path, std::uint64_t block = 10000)
        : s_(std::make_shared<state>())
    {
        s_->path = std::move(path);
        s_->block = block ? block : 1;
        s_->reserve();
    }

    // The fast path: no lock, no syscall, until a block runs out
    std::uint64_t next() const
    {
        state& st = *s_;
        for (;;) {
            const std::uint64_t v = st.cursor.fetch_add(1, std::memory_order_relaxed);
            if (v < st.limit.load(std::memory_order_acquire))
                return v;
            std::lock_guard<std::mutex> g(st.m);
            // Another thread may have refilled while we waited for the lock
            if (st.cursor.load(std::memory_order_relaxed) >=
                st.limit.load(std::memory_order_acquire))
                st.reserve();
        }
    }

    // Hand this to logger_config::sequence
    std::function<std::uint64_t()> source() const
    {
        auto st = s_;
        return [st] { return persistent_sequence(st).next(); };
    }

private:
    struct state {
        std::string path;
        std::uint64_t block = 10000;
        std::atomic<std::uint64_t> cursor{0};
        std::atomic<std::uint64_t> limit{0};
        std::mutex m;

        // Called with m held, or before any thread exists
        void reserve()
        {
            std::uint64_t start = 0;
            if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
                unsigned long long v = 0;
                if (std::fscanf(f, "%llu", &v) == 1)
                    start = v;
                std::fclose(f);
            }
            const std::uint64_t from = start > cursor.load(std::memory_order_relaxed)
                                           ? start
                                           : cursor.load(std::memory_order_relaxed);
            // Persist the END of the block before handing out any of it. If we
            // die mid-block the file already says those numbers are spent, so
            // the next process starts past them.
            if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
                std::fprintf(f, "%llu\n", (unsigned long long)(from + block));
                std::fflush(f);
                std::fclose(f);
            }
            cursor.store(from, std::memory_order_relaxed);
            limit.store(from + block, std::memory_order_release);
        }
    };

    explicit persistent_sequence(std::shared_ptr<state> s) : s_(std::move(s)) {}

    std::shared_ptr<state> s_;
};

// --------------------------------------------------------------- ordering
//
// Records carry a total order in `seq`, but they do not *arrive* in it. A
// thread can be descheduled between stamping its sequence number and queueing
// the record, so a lane routinely sees 7 before 6. For a file you can sort
// afterwards; for a live stream to four continents you cannot, and an operator
// watching telemetry arrive out of order will not trust any of it.
//
// This is the fix: a bounded reordering window in front of a sink. Records are
// held until the next expected sequence arrives, then released in order.
//
// The bound is what makes it safe rather than a stall. A lane with a drop
// policy will genuinely never deliver some sequence numbers, so waiting for a
// hole to fill would wait forever - once the window is full the buffer jumps
// to the lowest sequence it holds, counts the jump, and carries on. Ordering
// you can rely on, latency you can bound, and gaps you can see.
//
//     auto ordered = ordering_buffer(json_sink(send), 1024);
//     lane.add_sink(ordered);
//     ...
//     ordered.flush();          // release anything still held, in order
//
// Cost: at most `window` records of memory and however long it takes the
// missing sequence to show up. Sinks that do not care - a console, a file you
// will sort later - should not pay it.
class ordering_buffer {
public:
    // window: how far out of order the stream may be before a hole is
    // declared permanent and jumped. warmup: how many records to hold before
    // choosing the starting sequence - see operator() for why that matters.
    explicit ordering_buffer(std::function<void(const record&)> downstream,
                             std::size_t window = 1024, std::size_t warmup = 64)
        : s_(std::make_shared<state>())
    {
        s_->down = std::move(downstream);
        s_->window = window ? window : 1;
        s_->warmup = warmup ? warmup : 1;
    }

    // Called on the lane's drain thread, which is the only caller, so there is
    // no locking here
    void operator()(const record& r) const
    {
        state& st = *s_;
        if (st.started && r.seq < st.next) {    // genuinely late: emit rather than drop
            st.down(r);
            st.late++;
            return;
        }
        st.pending.emplace(r.seq, r);
        if (!st.started) {
            // Warm up before choosing where to start. Anchoring on the *first*
            // record to arrive is wrong precisely because arrival order is what
            // this class exists to correct: if 5 lands before 3, anchoring on 5
            // declares 3 late and emits it out of order. Holding a few records
            // first means the anchor is the smallest sequence actually in
            // flight, not the luckiest one.
            if (st.pending.size() < st.warmup)
                return;
            st.next = st.pending.begin()->first;
            st.started = true;
        }
        release_contiguous(st);
        // A hole that will never be filled must not stall the stream
        while (st.pending.size() > st.window) {
            st.next = st.pending.begin()->first;
            st.jumped++;
            release_contiguous(st);
        }
    }

    // Release everything still held, in order. For shutdown.
    void flush() const
    {
        state& st = *s_;
        st.started = true;                      // a short stream that never warmed up
        while (!st.pending.empty()) {
            st.next = st.pending.begin()->first;
            release_contiguous(st);
        }
    }

    std::uint64_t jumped() const noexcept { return s_->jumped; }

private:
    struct state {
        std::function<void(const record&)> down;
        std::map<std::uint64_t, record> pending;
        std::uint64_t next = 0;
        std::uint64_t jumped = 0, late = 0;
        std::size_t window = 1024;
        std::size_t warmup = 64;
        bool started = false;
    };

    static void release_contiguous(state& st)
    {
        while (!st.pending.empty() && st.pending.begin()->first == st.next) {
            st.down(st.pending.begin()->second);
            st.pending.erase(st.pending.begin());
            ++st.next;
        }
    }

    std::shared_ptr<state> s_;
};

namespace detail {

// One lane: a queue, a thread and a set of sinks. Everything a lane owns is
// private to it, which is the point - a lane can be slow, or full, or dropping
// records, without any of that reaching another lane.
struct lane_state {
    lane_config cfg;
    mpmc_queue<std::shared_ptr<const record>> q;
    moveable_signal<const record&> sig;
    // Installed when cfg.ordered_window is non-zero. The drain thread is the
    // only user, so there is no locking here.
    std::unique_ptr<ordering_buffer> reorder;
    std::vector<scoped_connection> kept;         // sinks, parked; see connect-and-park
    std::atomic<bool> running{false};
    std::atomic<std::uint64_t> pushed{0}, emitted{0}, dropped{0};
    std::thread drain;

    explicit lane_state(lane_config c)
        : cfg(std::move(c)), q(cfg.capacity ? cfg.capacity : 1024)
    {
        if (cfg.ordered_window)
            reorder.reset(new ordering_buffer([this](const record& r) { sig(r); },
                                              cfg.ordered_window));
    }

    ~lane_state() { stop(); }

    void start()
    {
        running.store(true, std::memory_order_release);
        drain = std::thread([this] { run(); });
    }

    void stop()
    {
        if (!running.exchange(false, std::memory_order_acq_rel))
            return;
        if (drain.joinable())
            drain.join();
        drain_remaining();                       // nothing queued is lost on exit
    }

    bool submit(const std::shared_ptr<const record>& r)
    {
        if (q.push(r)) {
            pushed.fetch_add(1, std::memory_order_release);
            return true;
        }
        switch (cfg.on_full) {
        case overflow::drop_oldest: {
            std::shared_ptr<const record> victim;
            if (q.try_pop(victim)) {             // make room, and count the loss
                dropped.fetch_add(1, std::memory_order_relaxed);
                emitted.fetch_add(1, std::memory_order_release);
                if (q.push(r)) {
                    pushed.fetch_add(1, std::memory_order_release);
                    return true;
                }
            }
            dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        case overflow::block:
            // Available and deliberately not the default: this couples the
            // producer's latency to this lane's slowest sink. Right for an
            // audit trail, wrong for a dashboard.
            while (running.load(std::memory_order_acquire) && !q.push(r))
                std::this_thread::yield();
            pushed.fetch_add(1, std::memory_order_release);
            return true;
        default:
            dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    void run()
    {
        std::shared_ptr<const record> r;
        while (running.load(std::memory_order_acquire)) {
            bool any = false;
            for (int i = 0; i < 256 && q.try_pop(r); ++i) {   // one wake, many records
                emit(*r);
                emitted.fetch_add(1, std::memory_order_release);
                any = true;
            }
            if (!any)
                std::this_thread::sleep_for(cfg.idle_sleep);
        }
    }

    void drain_remaining()
    {
        std::shared_ptr<const record> r;
        while (q.try_pop(r)) {
            emit(*r);
            emitted.fetch_add(1, std::memory_order_release);
        }
        // The lane knows when its stream has ended, so nobody has to remember
        // to release what the reordering window is still holding
        if (reorder)
            reorder->flush();
    }

    void emit(const record& r)
    {
        if (reorder)
            (*reorder)(r);
        else
            sig(r);
    }
};

} // namespace detail

// A handle to one lane. Moveable and copyable - it is a handle, not the lane.
class lane {
public:
    lane() = default;
    explicit lane(std::shared_ptr<detail::lane_state> s) : s_(std::move(s)) {}

    // Sinks are slots, parked so they live exactly as long as the lane
    template <typename F>
    lane& add_sink(F&& sink)
    {
        s_->kept.push_back(scoped_connection{s_->sig.connect(std::forward<F>(sink))});
        return *this;
    }

    const std::string& name() const noexcept { return s_->cfg.name; }
    std::uint64_t dropped() const noexcept { return s_->dropped.load(std::memory_order_relaxed); }
    std::uint64_t emitted() const noexcept { return s_->emitted.load(std::memory_order_relaxed); }
    std::size_t queued() const noexcept { return s_->q.size(); }
    // Gaps this lane's reordering window gave up waiting for, 0 if unordered
    std::uint64_t gaps_jumped() const noexcept
    {
        return s_->reorder ? s_->reorder->jumped() : 0;
    }

    // Block until everything queued at the moment of the call has been emitted
    void flush() const
    {
        if (!s_)
            return;
        const std::uint64_t target = s_->pushed.load(std::memory_order_acquire);
        while (s_->emitted.load(std::memory_order_acquire) < target &&
               s_->running.load(std::memory_order_acquire))
            std::this_thread::yield();
    }

    explicit operator bool() const noexcept { return s_ != nullptr; }

private:
    std::shared_ptr<detail::lane_state> s_;
};

class logger {
public:
    explicit logger(logger_config cfg = logger_config{})
        : c_(std::make_shared<core>(std::move(cfg)))
    {
        // A default lane exists from the start, so `lg.add_sink(...)` works
        // without anyone having to learn what a lane is first
        c_->cfg.default_lane.name = "default";
        c_->lanes.push_back(std::make_shared<detail::lane_state>(c_->cfg.default_lane));
        c_->lanes.back()->start();
    }

    logger(logger&&) noexcept = default;
    logger& operator=(logger&&) noexcept = default;
    logger(const logger&) = delete;
    logger& operator=(const logger&) = delete;

    // ---- the call-site test
    //
    // Relaxed on purpose: a level change is not a synchronisation point, and
    // paying for one on every disabled log call would defeat the test.
    bool enabled(level l) const noexcept
    {
        if (!c_)
            return false;
        const level cur = c_->level.load(std::memory_order_relaxed);
        return cur != level::off && l >= cur;
    }

    void set_level(level l) noexcept { c_->level.store(l, std::memory_order_relaxed); }
    level get_level() const noexcept { return c_->level.load(std::memory_order_relaxed); }
    const std::string& name() const noexcept { return c_->cfg.name; }

    // The ordering stamp for the next record. One relaxed fetch_add on the
    // default path; a configured source may do more, but not per record.
    std::uint64_t next_seq() const
    {
        return c_->cfg.sequence ? c_->cfg.sequence() : detail::next_seq();
    }

    // ---- lanes
    //
    // A lane is a queue and a thread of its own. Give the slow sinks their own
    // and they can only ever fall behind by themselves.
    lane add_lane(lane_config cfg)
    {
        auto st = std::make_shared<detail::lane_state>(std::move(cfg));
        st->start();
        c_->lanes.push_back(st);
        return lane(st);
    }
    lane add_lane(std::string name, std::size_t capacity = 8192,
                  overflow on_full = overflow::drop_newest,
                  std::size_t ordered_window = 0)
    {
        lane_config cfg;
        cfg.name = std::move(name);
        cfg.capacity = capacity;
        cfg.on_full = on_full;
        cfg.ordered_window = ordered_window;
        return add_lane(std::move(cfg));
    }

    // A lane whose sinks see records in sequence order, flushed for you
    lane add_ordered_lane(std::string name, std::size_t capacity = 8192,
                          std::size_t window = 4096,
                          overflow on_full = overflow::drop_oldest)
    {
        return add_lane(std::move(name), capacity, on_full, window);
    }

    lane default_lane() const { return lane(c_->lanes.front()); }

    // Shorthand for the common case: a sink on the default lane
    template <typename F>
    logger& add_sink(F&& sink)
    {
        default_lane().add_sink(std::forward<F>(sink));
        return *this;
    }

    std::size_t lane_count() const noexcept { return c_->lanes.size(); }

    // ---- writing, from any thread
    bool write(record r) const
    {
        if (!c_)
            return false;
        r.logger = c_->cfg.name;
        // Built once, shared by every lane: fanning out to five lanes is five
        // refcount bumps, not five copies of the message
        auto shared = std::make_shared<const record>(std::move(r));
        bool all = true;
        for (auto& l : c_->lanes)
            all = l->submit(shared) && all;
        return all;
    }

    bool write(level l, std::string message, const char* file = "", int line = 0) const
    {
        record r;
        r.seq = next_seq();
        r.level = l;
        r.when = std::chrono::system_clock::now();
        r.thread = std::this_thread::get_id();
        r.file = file;
        r.line = line;
        r.message = std::move(message);
        return write(std::move(r));
    }

    // Telemetry on the same pipeline, so every sink already attached gets it
    bool metric(std::string name, double value,
                std::vector<std::pair<std::string, std::string>> tags = {}) const
    {
        record r;
        r.seq = next_seq();
        r.level = level::info;
        r.when = std::chrono::system_clock::now();
        r.thread = std::this_thread::get_id();
        r.message = std::move(name);
        r.value = value;
        r.fields = std::move(tags);
        r.is_metric = true;
        return write(std::move(r));
    }

    // ---- lifecycle and introspection
    void flush() const
    {
        for (auto& l : c_->lanes)
            lane(l).flush();
    }

    std::uint64_t dropped() const noexcept
    {
        std::uint64_t n = 0;
        for (auto& l : c_->lanes)
            n += l->dropped.load(std::memory_order_relaxed);
        return n;
    }
    std::uint64_t emitted() const noexcept
    {
        std::uint64_t n = 0;
        for (auto& l : c_->lanes)
            n += l->emitted.load(std::memory_order_relaxed);
        return n;
    }

private:
    struct core {
        logger_config cfg;
        std::atomic<level> level;
        std::vector<std::shared_ptr<detail::lane_state>> lanes;

        explicit core(logger_config c) : cfg(std::move(c)), level(cfg.level) {}
        // lane_state's destructor stops and drains its own thread, so there is
        // nothing to orchestrate here
    };

    std::shared_ptr<core> c_;
};

// The process-wide logger, for code that should not have to be handed one
namespace detail {

// Stream-style call sites. The record is submitted when the temporary dies at
// the end of the full expression, so `LOG_INFO() << a << b;` is one record.
class record_builder {
public:
    record_builder(const logger& lg, level l, const char* file, int line)
        : lg_(&lg)
    {
        // Stamped at the call site, not at submission: the order is the order
        // things happened, not the order they reached a queue
        r_.seq = lg.next_seq();
        r_.level = l;
        r_.when = std::chrono::system_clock::now();
        r_.thread = std::this_thread::get_id();
        r_.file = file;
        r_.line = line;
    }

    record_builder(const record_builder&) = delete;
    record_builder& operator=(const record_builder&) = delete;
    record_builder(record_builder&&) = default;

    ~record_builder()
    {
        if (!lg_)
            return;
        r_.message = os_.str();
        lg_->write(std::move(r_));
    }

    template <typename T>
    record_builder& operator<<(const T& v)
    {
        os_ << v;
        return *this;
    }

    // Structured extras, which is what makes the JSON sink worth having
    // Route this record to a stream: aircraft 7, tenant 3, session 42
    record_builder& channel(int ch)
    {
        r_.channel = ch;
        return *this;
    }

    template <typename T>
    record_builder& field(std::string key, const T& v)
    {
        std::ostringstream tmp;
        tmp << v;
        r_.fields.emplace_back(std::move(key), tmp.str());
        return *this;
    }

private:
    const logger* lg_;
    record r_;
    std::ostringstream os_;
};

} // namespace detail

// ------------------------------------------------------------------- sinks
//
// A sink is a slot: any callable taking `const record&`. These are the ones
// worth shipping; anything else is three lines at the call site.

// Human-readable, coloured when the destination is a terminal
inline std::function<void(const record&)> console_sink(std::FILE* out = stderr,
                                                       bool colour = true)
{
    return [out, colour](const record& r) {
        static const char* const colours[] = {
            "\033[90m", "\033[36m", "\033[32m", "\033[33m", "\033[31m", "\033[1;31m"};
        const int idx = static_cast<int>(r.level);
        const bool tty = colour && SN_LOG_ISATTY(SN_LOG_FILENO(out));
        std::string line;
        line += detail::format_time(r.when);
        char seqbuf[24];
        std::snprintf(seqbuf, sizeof seqbuf, " #%llu ", (unsigned long long)r.seq);
        line += seqbuf;
        if (tty && idx >= 0 && idx < 6) {
            line += colours[idx];
            line += to_string(r.level);
            line += "\033[0m";
        } else {
            line += to_string(r.level);
        }
        line += " [";
        line += r.logger;
        line += "] ";
        line += r.message;
        if (r.is_metric) {
            char v[32];
            std::snprintf(v, sizeof v, "=%g", r.value);
            line += v;
        }
        for (const auto& f : r.fields) {
            line += ' ';
            line += f.first;
            line += '=';
            line += f.second;
        }
        if (r.file && *r.file) {
            line += " (";
            line += detail::basename(r.file);
            line += ':';
            line += std::to_string(r.line);
            line += ')';
        }
        line += '\n';
        std::fwrite(line.data(), 1, line.size(), out);
        if (r.level >= level::error)
            std::fflush(out);                    // never lose the last error to a crash
    };
}

// One JSON object per record, handed to whatever you like: a file, a socket,
// a WebSocket broadcast. Keeping the transport out of here is what lets the
// same sink feed a browser and a database.
// One record, one JSON object. Exposed on its own so a fan-out serialises
// once and sends the same bytes everywhere - doing it per destination is how a
// relay to four continents becomes the bottleneck.
inline std::string to_json(const record& r)
{
    {
        std::string j = "{\"ts\":\"";
        j += detail::format_time(r.when);
        j += "\",\"seq\":";
        j += std::to_string(r.seq);            // the field to sort by
        j += ",\"level\":\"";
        j += to_string(r.level);
        j += "\",\"logger\":\"";
        detail::json_escape(r.logger, j);
        j += "\",\"msg\":\"";
        detail::json_escape(r.message, j);
        j += '"';
        if (r.is_metric) {
            char v[40];
            std::snprintf(v, sizeof v, ",\"value\":%.17g", r.value);
            j += v;
        }
        if (r.channel >= 0) {
            j += ",\"ch\":";
            j += std::to_string(r.channel);
        }
        if (r.file && *r.file) {
            j += ",\"src\":\"";
            detail::json_escape(detail::basename(r.file), j);
            j += ':';
            j += std::to_string(r.line);
            j += '"';
        }
        for (const auto& f : r.fields) {
            j += ",\"";
            detail::json_escape(f.first, j);
            j += "\":\"";
            detail::json_escape(f.second, j);
            j += '"';
        }
        j += "}";
        return j;
    }
}

inline std::function<void(const record&)> json_sink(std::function<void(const std::string&)> out)
{
    return [out = std::move(out)](const record& r) { out(to_json(r)); };
}

// Append to a file. Opened once, flushed on error and on close; the drain
// thread is the only writer, so there is no locking here at all.
inline std::function<void(const record&)> file_sink(const std::string& path)
{
    std::shared_ptr<std::FILE> state(std::fopen(path.c_str(), "ab"),
                                     [](std::FILE* f) { if (f) std::fclose(f); });
    return [state](const record& r) {
        std::FILE* f = state.get();
        if (!f)
            return;
        std::string line = detail::format_time(r.when);
        line += " #";
        line += std::to_string(r.seq);
        line += ' ';
        line += to_string(r.level);
        line += " [";
        line += r.logger;
        line += "] ";
        line += r.message;
        for (const auto& fl : r.fields) {
            line += ' ';
            line += fl.first;
            line += '=';
            line += fl.second;
        }
        line += '\n';
        std::fwrite(line.data(), 1, line.size(), f);
        if (r.level >= level::error)
            std::fflush(f);
    };
}

// The process-wide logger, for code that should not have to be handed one.
// It arrives with a console sink already attached, so the very first thing a
// developer writes - SN_LOG_INFO() << "hello" - prints, without a setup step
// they had to read about first. Replace or add sinks whenever you like.
inline logger& default_logger()
{
    static logger lg = [] {
        logger_config c;
        c.name = "app";
        logger l{std::move(c)};
        l.add_sink(console_sink());
        return l;
    }();
    return lg;
}

// ---------------------------------------------------------------- replay
//
// Training. An incident happened at 14:02 last Tuesday; a trainee in New York
// needs to watch it unfold the way the operator did, at the speed it actually
// happened, into the same console and the same dashboard.
//
// That works here for the same reason the event loop is replayable: a record
// is a value, sinks are ordinary slots, and nothing downstream can tell a
// replayed record from a live one. So the training console is the production
// console, unmodified.
//
//     journal j;
//     lane.add_sink(j.sink());                 // capture, live
//     ...
//     replayer(j.take()).play(console_sink());             // as it happened
//     replayer(j.take()).play(to_ws, 4.0);                 // four times faster
//     replayer(j.take()).play(to_ws, 0.0);                 // as fast as possible
//
// Pacing uses the nanosecond timestamps, so gaps are reproduced: the twelve
// seconds where nothing happened before the battery warning are twelve
// seconds of nothing, which is exactly what a trainee needs to feel.

class journal {
public:
    journal() : s_(std::make_shared<state>()) {}

    // Attach to any lane. Ordered lanes give an ordered journal for free.
    std::function<void(const record&)> sink() const
    {
        auto st = s_;
        return [st](const record& r) {
            std::lock_guard<std::mutex> g(st->m);
            st->records.push_back(r);
        };
    }

    std::size_t size() const
    {
        std::lock_guard<std::mutex> g(s_->m);
        return s_->records.size();
    }

    std::vector<record> take() const
    {
        std::lock_guard<std::mutex> g(s_->m);
        return s_->records;
    }

private:
    struct state {
        mutable std::mutex m;
        std::vector<record> records;
    };
    std::shared_ptr<state> s_;
};

class replayer {
public:
    explicit replayer(std::vector<record> recs) : recs_(std::move(recs))
    {
        // A journal from an unordered lane may be shuffled; a replay that is
        // not in order is not a replay of anything
        std::sort(recs_.begin(), recs_.end(),
                  [](const record& a, const record& b) { return a.seq < b.seq; });
    }

    // speed 1.0 replays at the original pace; 4.0 four times faster; 0 means
    // as fast as the sink will take it. stop, if given, ends the replay early.
    void play(const std::function<void(const record&)>& out, double speed = 1.0,
              const std::atomic<bool>* stop = nullptr) const
    {
        if (recs_.empty())
            return;
        const auto origin = recs_.front().when;
        const auto wall_start = std::chrono::steady_clock::now();
        for (const record& r : recs_) {
            if (stop && stop->load(std::memory_order_acquire))
                return;
            if (speed > 0.0) {
                // Pace against the ORIGINAL offset, not the previous record:
                // sleeping for each gap in turn accumulates the error of every
                // sleep, and a long replay drifts badly
                const auto offset = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    r.when - origin);
                const auto scaled = std::chrono::nanoseconds(
                    static_cast<std::int64_t>(double(offset.count()) / speed));
                const auto due = wall_start + scaled;
                std::this_thread::sleep_until(due);
            }
            out(r);
        }
    }

    // One record at a time, for stepping through an incident
    bool step(const std::function<void(const record&)>& out)
    {
        if (at_ >= recs_.size())
            return false;
        out(recs_[at_++]);
        return true;
    }

    void rewind() noexcept { at_ = 0; }
    std::size_t size() const noexcept { return recs_.size(); }

    // How long the original session lasted
    std::chrono::nanoseconds duration() const
    {
        if (recs_.size() < 2)
            return std::chrono::nanoseconds{0};
        return std::chrono::duration_cast<std::chrono::nanoseconds>(recs_.back().when -
                                                                    recs_.front().when);
    }

private:
    std::vector<record> recs_;
    std::size_t at_ = 0;
};

} // namespace log
} // namespace snicholls

// The call sites. The level test happens before anything is built, so a
// disabled log statement costs one relaxed atomic load and a branch.
//
// The `if (!x) ; else` shape is deliberate: it keeps the macro safe inside an
// unbraced `if` without swallowing a following `else`.
#define SN_LOG_AT(lg, lvl)                                                              \
    if (!(lg).enabled(lvl)) ;                                                           \
    else ::snicholls::log::detail::record_builder((lg), (lvl), __FILE__, __LINE__)

#define SN_LOGGER_TRACE(lg)    SN_LOG_AT(lg, ::snicholls::log::level::trace)
#define SN_LOGGER_DEBUG(lg)    SN_LOG_AT(lg, ::snicholls::log::level::debug)
#define SN_LOGGER_INFO(lg)     SN_LOG_AT(lg, ::snicholls::log::level::info)
#define SN_LOGGER_WARN(lg)     SN_LOG_AT(lg, ::snicholls::log::level::warn)
#define SN_LOGGER_ERROR(lg)    SN_LOG_AT(lg, ::snicholls::log::level::error)
#define SN_LOGGER_CRITICAL(lg) SN_LOG_AT(lg, ::snicholls::log::level::critical)

// The same, on the process-wide logger, for code that should not have to be
// handed one
#define SN_LOG_TRACE()    SN_LOGGER_TRACE(::snicholls::log::default_logger())
#define SN_LOG_DEBUG()    SN_LOGGER_DEBUG(::snicholls::log::default_logger())
#define SN_LOG_INFO()     SN_LOGGER_INFO(::snicholls::log::default_logger())
#define SN_LOG_WARN()     SN_LOGGER_WARN(::snicholls::log::default_logger())
#define SN_LOG_ERROR()    SN_LOGGER_ERROR(::snicholls::log::default_logger())
#define SN_LOG_CRITICAL() SN_LOGGER_CRITICAL(::snicholls::log::default_logger())

#endif /* logging_hpp */
