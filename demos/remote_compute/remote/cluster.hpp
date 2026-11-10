//
//  remote/cluster.hpp
//  TSMoveables - demos/remote_compute (from the remote_compute_example prototype at 92d8dc4)
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Run a kernel on another machine and get a std::future back - or, for a long
//  job, a handle you can watch and cancel.
//
//      const auto key = snicholls::remote::load_key();              // shared by every machine
//
//      snicholls::remote::worker<kernels> w(key, "pi-3", "0.0.0.0", 7400);   // on the Pi
//      w.advertise();                                                        // announce on the LAN
//
//      snicholls::remote::cluster<kernels> c(key);                  // on the Mac
//      c.discover();                                                // add workers as they appear
//      c.add("mac-studio", "192.168.1.20", 7400);                   // or name them
//
//      double p = c.invoke(on{"mac-studio"}, price, spec, 50'000'000);      // like std::invoke
//      std::future<double> f = c.async(on{"mac-studio"}, price, spec, 50'000'000);
//      auto j = c.submit(on{"burrawang"}, long_price, spec, 2'000'000'000);
//      j.progress();  j.cancel();  c.health();
//
//  ------------------------------------------------------------------ security
//
//  Every machine holds the same 32-byte key (remote_demo keygen writes one).
//  On connect both sides prove they hold it - a mutual HMAC-SHA256 challenge
//  over fresh random nonces - so a worker runs nothing for a caller without
//  the key, and a client sends nothing to a machine pretending to be a worker.
//  Beacons are signed with it too. Nothing is encrypted: that needs TLS, and
//  TLS needs OpenSSL on every machine.
//
//  -------------------------------------------------- sessions and reconnection
//
//  A client's calls belong to a session, not a connection. When the connection
//  drops - Wi-Fi blinks, a laptop sleeps, a machine stops answering pings - the
//  worker keeps running the session's jobs and keeps their results, and the
//  client keeps reconnecting with backoff. On reconnect the worker says which
//  calls it knows: those are waited for (their results arrive, kept or fresh),
//  and any the worker never received are sent again, which is still
//  at-most-once because they never ran. A result is kept until the client acks
//  it. A session nobody resumes within resume_window (60 s) is ended on both
//  sides: the worker cancels its jobs and the client fails them.
//
//  A worker that shuts down says so (bye), and its calls fail at once rather
//  than waiting out the window. A call is never re-run on another machine.
//
//  ------------------------------------------------------- long-running jobs
//
//  Inside a kernel, this_job reports progress and checks for cancellation:
//
//      this_job::throw_if_cancelled();
//      this_job::progress(double(done) / total, "840M paths");
//
//  Workers answer ping on the connection's own thread, never the job pool, so a
//  worker with every core busy still answers within a network round trip.
//
//  PROTOTYPE: moves into ts-moveables (workers, cluster) and core (kernels,
//  signatures) after the core migration.
//

#ifndef remote_cluster_hpp
#define remote_cluster_hpp

#include "discovery.hpp"
#include "kernel.hpp"
#include "sha256.hpp"
#include "wire.hpp"

#include "concurrent/thread_pool.hpp"   // ts-moveables
#include "net/tcp.hpp"                  // ts-moveables

#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace snicholls::remote {

using namespace std::chrono_literals;
using steady = std::chrono::steady_clock;

/// Every remote failure, as seen through std::future::get().
struct remote_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// The job was cancelled. Thrown inside the kernel by this_job::throw_if_cancelled(),
/// and from the client's get().
struct job_cancelled : remote_error {
    using remote_error::remote_error;
};

/// What a worker says about itself when a client connects.
struct node_info {
    std::string name;
    std::uint32_t cores = 0;
    std::string arch;
    std::string host;
};

/// A worker's answer to a ping.
struct node_health {
    std::uint64_t uptime_ms = 0;
    std::uint32_t cores = 0;
    std::uint32_t running = 0;     ///< jobs executing now, all clients
    std::uint64_t completed = 0;   ///< jobs finished, all clients
    std::uint64_t failed = 0;      ///< jobs that threw or were cancelled
    double load1 = 0;              ///< the OS one-minute load average
};

/// The latest progress a job reported.
struct job_progress {
    double fraction = 0;    ///< 0 to 1, as the kernel reported it
    std::string note;       ///< free text from the kernel
    steady::time_point at;  ///< when it arrived; default-constructed if none yet
};

// =========================================================================== keys

/// Where the shared key lives: $SNICHOLLS_REMOTE_KEY (hex), else ~/.snicholls_remote_key.
inline std::string key_path()
{
    const char* home = std::getenv("HOME");
    return std::string(home != nullptr ? home : ".") + "/.snicholls_remote_key";
}

/// Load the shared key. Throws with instructions if there is none.
inline std::string load_key()
{
    if (const char* env = std::getenv("SNICHOLLS_REMOTE_KEY"); env != nullptr && *env != '\0')
        return from_hex(env);
    std::ifstream in(key_path());
    std::string hex;
    if (!(in >> hex) || hex.size() < 32)
        throw std::runtime_error("no shared key: run `remote_demo keygen` on one machine and copy " + key_path() +
                                 " to every other, or set SNICHOLLS_REMOTE_KEY");
    return from_hex(hex);
}

/// Write a new random key to key_path(), readable by its owner only.
inline std::string generate_key_file()
{
    const std::string hex = to_hex(random_bytes(32));
    {
        std::ofstream out(key_path(), std::ios::trunc);
        out << hex << "\n";
    }
    ::chmod(key_path().c_str(), 0600);
    return hex;
}

/// Like std::this_thread: inside a kernel, which node is running it.
namespace this_node {
namespace detail {
inline thread_local std::string current_name;
}
/// The name of the worker running the current kernel; empty outside one.
inline const std::string& name() noexcept { return detail::current_name; }
} // namespace this_node

namespace detail {

enum class err_code : std::uint8_t { failed = 0, cancelled = 1, refused = 2 };

// What the worker knows about one running job, reachable from inside the kernel
struct job_context {
    std::function<void(double, std::string_view)> report;   // sends a prog frame
    std::atomic<bool> cancel{false};
    steady::time_point started = steady::now();
    steady::time_point last_report{};
};

inline thread_local job_context* current_job = nullptr;

} // namespace detail

/// Inside a kernel: report progress, check for cancellation.
namespace this_job {

/// Report progress, 0 to 1, with an optional note. Throttled to five updates a
/// second (the first and a final 1.0 always go), so it is cheap in a loop.
/// Outside a remote kernel it does nothing.
inline void progress(double fraction, std::string_view note = {})
{
    auto* j = detail::current_job;
    if (j == nullptr)
        return;
    const auto now = steady::now();
    if (fraction < 1.0 && j->last_report != steady::time_point{} && now - j->last_report < 200ms)
        return;
    j->last_report = now;
    j->report(fraction, note);
}

/// Whether the client has asked for this job to stop.
inline bool cancelled() noexcept
{
    const auto* j = detail::current_job;
    return j != nullptr && j->cancel.load(std::memory_order_relaxed);
}

/// Throw job_cancelled if the client has asked for this job to stop.
inline void throw_if_cancelled()
{
    if (cancelled())
        throw job_cancelled("cancelled");
}

/// How long this job has been running.
inline steady::duration elapsed() noexcept
{
    const auto* j = detail::current_job;
    return j == nullptr ? steady::duration{} : steady::now() - j->started;
}

} // namespace this_job

namespace detail {

inline std::string arch_name()
{
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__arm__)
    return "arm32";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86-64";
#else
    return "unknown";
#endif
}

inline std::string host_name()
{
    char buf[256] = {};
    return ::gethostname(buf, sizeof buf - 1) == 0 ? std::string(buf) : std::string("?");
}

inline double load_average()
{
    double l[1] = {0};
    return ::getloadavg(l, 1) == 1 ? l[0] : -1.0;
}

// Changes whenever the handshake or the health record changes shape
inline constexpr std::uint64_t protocol_signature =
    signature_v<std::tuple<node_info, node_health, std::string, std::vector<std::uint64_t>>> ^ 0x2;

enum class read_status { frame, timeout, closed };

// Read until one whole frame is buffered. Says which of the three happened
// rather than leaving it to errno: a clean close returns 0 bytes and sets no
// errno at all, so a stale ETIMEDOUT from an earlier poll would make a closed
// connection look like a quiet one and the reader would wait on it forever.
inline read_status read_frame_status(net::tcp_stream& s, std::string& buf, frame& f, std::size_t& used,
                                     std::chrono::milliseconds timeout)
{
    for (;;) {
        if (!buf.empty()) {
            used = parse_frame(buf, f);
            if (used != 0)
                return read_status::frame;
        }
        char chunk[64 * 1024];
        errno = 0;
        const auto n = s.read(chunk, sizeof chunk, timeout);
        if (n == 0)
            return read_status::closed;
        if (n < 0)
            return errno == ETIMEDOUT ? read_status::timeout : read_status::closed;
        buf.append(chunk, static_cast<std::size_t>(n));
    }
}

inline bool read_frame(net::tcp_stream& s, std::string& buf, frame& f, std::size_t& used,
                       std::chrono::milliseconds timeout)
{
    return read_frame_status(s, buf, f, used, timeout) == read_status::frame;
}

inline std::uint64_t take_call_id(std::string_view& payload)
{
    reader r(payload);
    const auto id = r.get<std::uint64_t>();
    payload.remove_prefix(8);
    return id;
}

// The handshake MACs. Each side signs the other's fresh nonce, so a recorded
// handshake cannot be replayed, and the labels stop one side's proof being
// reflected back as the other's.
inline std::string client_proof(std::string_view key, std::string_view server_nonce, std::string_view client_nonce,
                                std::uint64_t session)
{
    std::string m = "snrem1 client ";
    m.append(server_nonce).append(client_nonce);
    encode(m, session);
    return std::string(as_view(hmac_sha256(key, m)));
}

inline std::string server_proof(std::string_view key, std::string_view server_nonce, std::string_view client_nonce)
{
    std::string m = "snrem1 server ";
    m.append(client_nonce).append(server_nonce);
    return std::string(as_view(hmac_sha256(key, m)));
}

inline std::string frame_bytes(frame_kind kind, std::string_view token, std::uint64_t sig, std::string_view payload)
{
    std::string out;
    append_frame(out, kind, token, sig, payload);
    return out;
}

// One TCP connection and the lock that keeps whole frames from interleaving
struct wire_conn {
    net::tcp_stream stream;
    std::mutex write_mutex;
    std::string buf;

    bool write(std::string_view bytes)
    {
        std::lock_guard lk(write_mutex);
        return stream.write_all(bytes.data(), bytes.size(), 10s);
    }
};

} // namespace detail

// ========================================================================= worker

/// Serves the kernels in Set to anyone who holds the key, on a pool of threads.
template <typename Set>
class worker {
public:
    worker(std::string key, std::string name, const std::string& host, std::uint16_t port,
           std::size_t threads = std::thread::hardware_concurrency(),
           std::chrono::milliseconds resume_window = 60s)
        : key_(std::move(key)), name_(std::move(name)), resume_window_(resume_window),
          pool_(threads == 0 ? 1 : threads)
    {
        if (key_.size() < 16)
            throw remote_error("worker " + name_ + ": the shared key is missing or too short");
        std::string err;
        listener_ = net::tcp_listener::bind(host, port, 128, &err);
        if (!listener_.valid())
            throw remote_error("worker " + name_ + ": cannot listen on " + host + ":" + std::to_string(port) +
                               ": " + err);
        info_ = node_info{name_, static_cast<std::uint32_t>(pool_.worker_count()), detail::arch_name(),
                          detail::host_name()};
        acceptor_ = std::thread([this] { accept_loop(); });
    }

    worker(const worker&) = delete;
    worker& operator=(const worker&) = delete;

    ~worker() { stop(); }

    std::uint16_t port() const noexcept { return listener_.port(); }
    const std::string& name() const noexcept { return name_; }

    /// Announce this worker on the LAN, signed with the key, until stopped.
    void advertise(discovery_options opt = {})
    {
        beacon_ = std::make_unique<beacon>(
            key_, advertised{name_, "", port(), info_.cores, info_.arch, detail::protocol_signature}, std::move(opt));
    }

    node_health health() const
    {
        return {static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(steady::now() - started_).count()),
                info_.cores, running_.load(), completed_.load(), failed_.load(), detail::load_average()};
    }

    /// Drop every connection without a goodbye, as a network fault would. The
    /// sessions survive for the resume window. For tests and drills.
    void drop_connections()
    {
        std::lock_guard lk(conns_mutex_);
        for (auto& c : conns_)
            c->wire.stream.shutdown();
    }

    /// Say goodbye to every client, cancel every job, and wait for kernels to finish.
    void stop()
    {
        if (stopping_.exchange(true))
            return;
        beacon_.reset();
        listener_.shutdown();
        if (acceptor_.joinable())
            acceptor_.join();
        std::vector<std::shared_ptr<connection>> conns;
        {
            std::lock_guard lk(conns_mutex_);
            conns = conns_;
        }
        {
            std::lock_guard lk(sessions_mutex_);
            for (auto& [id, s] : sessions_)
                s->cancel_all();
        }
        std::string bye;
        encode(bye, std::string("worker " + name_ + " shut down"));
        const auto frame = detail::frame_bytes(frame_kind::bye, name_, 0, bye);
        for (auto& c : conns) {
            c->wire.write(frame);
            c->wire.stream.shutdown();
        }
        for (auto& c : conns)
            if (c->reader.joinable())
                c->reader.join();
        pool_.wait_idle();
    }

private:
    struct connection;

    // A client's calls, across however many connections it takes
    struct session {
        std::uint64_t id = 0;
        std::mutex m;
        std::shared_ptr<connection> conn;   // null while the client is away
        std::map<std::uint64_t, std::shared_ptr<detail::job_context>> running;
        std::map<std::uint64_t, std::string> results;   // complete ret/err frames, until acked
        steady::time_point detached_at = steady::now();

        void cancel_all()
        {
            std::lock_guard lk(m);
            for (auto& [id, j] : running)
                j->cancel = true;
        }
    };

    struct connection {
        detail::wire_conn wire;
        std::thread reader;
        std::shared_ptr<session> sess;   // set once authenticated
    };

    void accept_loop()
    {
        while (!stopping_) {
            reap_sessions();
            auto s = listener_.accept(200ms);
            if (!s.valid())
                continue;
            s.set_nodelay(true);
            auto c = std::make_shared<connection>();
            c->wire.stream = std::move(s);
            std::lock_guard lk(conns_mutex_);
            conns_.erase(std::remove_if(conns_.begin(), conns_.end(),
                                        [](const auto& x) { return !x->wire.stream.valid(); }),
                         conns_.end());
            c->reader = std::thread([this, c] { serve(c); });
            conns_.push_back(std::move(c));
        }
    }

    // Sessions nobody came back for: cancel their jobs and forget their results
    void reap_sessions()
    {
        std::lock_guard lk(sessions_mutex_);
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            std::unique_lock sl(it->second->m);
            if (it->second->conn == nullptr && steady::now() - it->second->detached_at > resume_window_) {
                for (auto& [id, j] : it->second->running)
                    j->cancel = true;
                sl.unlock();
                it = sessions_.erase(it);
            }
            else
                ++it;
        }
    }

    bool authenticate(const std::shared_ptr<connection>& c)
    {
        const std::string nonce = random_bytes(32);
        std::string hello;
        encode(hello, info_, nonce);
        if (!c->wire.write(detail::frame_bytes(frame_kind::hello, name_, detail::protocol_signature, hello)))
            return false;

        frame f;
        std::size_t used = 0;
        if (!detail::read_frame(c->wire.stream, c->wire.buf, f, used, 5s) || f.kind != frame_kind::auth)
            return false;
        reader r(f.payload);
        const auto session_id = r.get<std::uint64_t>();
        const auto client_nonce = r.get<std::string>();
        const auto proof = r.get<std::string>();
        c->wire.buf.erase(0, used);
        if (client_nonce.size() != 32 ||
            !equal_constant_time(proof, detail::client_proof(key_, nonce, client_nonce, session_id))) {
            std::string why;
            encode(why, std::string("worker " + name_ + " refused the connection: authentication failed (wrong key)"));
            c->wire.write(detail::frame_bytes(frame_kind::bye, name_, 0, why));
            return false;
        }

        std::shared_ptr<session> s;
        {
            std::lock_guard lk(sessions_mutex_);
            auto& slot = sessions_[session_id];
            if (!slot) {
                slot = std::make_shared<session>();
                slot->id = session_id;
            }
            s = slot;
        }

        // Attach under the session lock and send the welcome and every kept
        // result before releasing it, so no result can overtake the welcome
        std::lock_guard sl(s->m);
        if (s->conn && s->conn != c)
            s->conn->wire.stream.shutdown();   // a stale connection from the same client
        s->conn = c;
        c->sess = s;
        std::vector<std::uint64_t> known;
        for (const auto& [id, j] : s->running)
            known.push_back(id);
        for (const auto& [id, bytes] : s->results)
            known.push_back(id);
        std::string welcome;
        encode(welcome, detail::server_proof(key_, nonce, client_nonce), known);
        if (!c->wire.write(detail::frame_bytes(frame_kind::welcome, name_, detail::protocol_signature, welcome)))
            return false;
        for (const auto& [id, bytes] : s->results)
            c->wire.write(bytes);
        return true;
    }

    void serve(const std::shared_ptr<connection>& c)
    {
        try {
            if (authenticate(c)) {
                frame f;
                std::size_t used = 0;
                while (!stopping_) {
                    const auto st = detail::read_frame_status(c->wire.stream, c->wire.buf, f, used, 250ms);
                    if (st == detail::read_status::closed)
                        break;
                    if (st == detail::read_status::timeout)
                        continue;
                    switch (f.kind) {
                    case frame_kind::call: dispatch(c, f); break;
                    case frame_kind::ping: pong(c, f); break;
                    case frame_kind::cancel: cancel(c, f); break;
                    case frame_kind::ack: ack(c, f); break;
                    default: break;
                    }
                    c->wire.buf.erase(0, used);
                }
            }
        }
        catch (const std::exception&) {
            // A peer that sends garbage loses its connection; the worker carries on
        }
        if (c->sess) {
            std::lock_guard sl(c->sess->m);
            if (c->sess->conn == c) {
                c->sess->conn = nullptr;
                c->sess->detached_at = steady::now();
            }
        }
        c->wire.stream.shutdown();
    }

    // On the connection's thread, never the pool: a busy worker still answers
    void pong(const std::shared_ptr<connection>& c, const frame& f)
    {
        std::string out(f.payload);   // the nonce, echoed
        encode(out, health());
        c->wire.write(detail::frame_bytes(frame_kind::pong, name_, detail::protocol_signature, out));
    }

    void cancel(const std::shared_ptr<connection>& c, const frame& f)
    {
        std::string_view payload = f.payload;
        const auto id = detail::take_call_id(payload);
        std::lock_guard lk(c->sess->m);
        if (const auto it = c->sess->running.find(id); it != c->sess->running.end())
            it->second->cancel = true;
    }

    void ack(const std::shared_ptr<connection>& c, const frame& f)
    {
        std::string_view payload = f.payload;
        const auto id = detail::take_call_id(payload);
        std::lock_guard lk(c->sess->m);
        c->sess->results.erase(id);
    }

    // Send to whichever connection the session has now; keep it until acked either way
    void deliver(const std::shared_ptr<session>& s, std::uint64_t id, std::string bytes)
    {
        std::shared_ptr<connection> to;
        {
            std::lock_guard lk(s->m);
            s->running.erase(id);
            s->results[id] = bytes;
            to = s->conn;
        }
        if (to)
            to->wire.write(bytes);
    }

    void dispatch(const std::shared_ptr<connection>& c, const frame& f)
    {
        std::string_view payload = f.payload;
        const auto id = detail::take_call_id(payload);
        const std::string kernel(f.kernel);
        const auto s = c->sess;

        {
            std::lock_guard lk(s->m);
            if (s->running.count(id) != 0 || s->results.count(id) != 0)
                return;   // a resend of a call already here: at-most-once
        }

        const int k = Set::find(kernel);
        if (k < 0)
            return deliver(s, id, error_frame(kernel, id, detail::err_code::refused,
                                              "worker " + name_ + " has no kernel called '" + kernel + "'"));
        if (Set::signatures[k] != f.signature)
            return deliver(s, id, error_frame(kernel, id, detail::err_code::refused,
                                              "kernel '" + kernel + "' has a different signature on worker " + name_ +
                                                  ": the two machines were built from different code"));

        auto job = std::make_shared<detail::job_context>();
        job->report = [s, kernel, id](double fraction, std::string_view note) {
            std::string out;
            encode(out, id, fraction, std::string(note));
            std::shared_ptr<connection> to;
            {
                std::lock_guard lk(s->m);
                to = s->conn;
            }
            if (to)
                to->wire.write(detail::frame_bytes(frame_kind::prog, kernel, 0, out));
        };
        {
            std::lock_guard lk(s->m);
            s->running[id] = job;
        }

        pool_.submit([this, s, job, kernel, id, k, args = std::string(payload), sig = f.signature] {
            running_.fetch_add(1);
            std::string out;
            encode(out, id);
            detail::err_code code = detail::err_code::failed;
            std::string why;
            bool ok = false;
            this_node::detail::current_name = name_;
            detail::current_job = job.get();
            try {
                if (job->cancel)
                    throw job_cancelled("cancelled");
                Set::invokers[k](args, out);
                ok = true;
            }
            catch (const job_cancelled&) {
                code = detail::err_code::cancelled;
                why = "job '" + kernel + "' was cancelled on worker " + name_;
            }
            catch (const std::exception& e) {
                why = std::string("kernel '") + kernel + "' threw on worker " + name_ + ": " + e.what();
            }
            catch (...) {
                why = "kernel '" + kernel + "' threw a non-exception on worker " + name_;
            }
            detail::current_job = nullptr;
            this_node::detail::current_name.clear();
            running_.fetch_sub(1);
            (ok ? completed_ : failed_).fetch_add(1);
            deliver(s, id, ok ? detail::frame_bytes(frame_kind::ret, kernel, sig, out) : error_frame(kernel, id, code, why));
        });
    }

    std::string error_frame(const std::string& kernel, std::uint64_t id, detail::err_code code, const std::string& why)
    {
        std::string out;
        encode(out, id, static_cast<std::uint8_t>(code), why);
        return detail::frame_bytes(frame_kind::err, kernel, 0, out);
    }

    std::string key_;
    std::string name_;
    std::chrono::milliseconds resume_window_;
    node_info info_;
    steady::time_point started_ = steady::now();
    std::atomic<std::uint32_t> running_{0};
    std::atomic<std::uint64_t> completed_{0};
    std::atomic<std::uint64_t> failed_{0};
    net::tcp_listener listener_;
    // mutex_task_pool, not work_stealing_task_pool: a worker is long-lived and
    // mostly idle, and work-stealing workers yield-spin when idle (by design,
    // for latency). Left running, that is every core at 100% doing nothing.
    // These threads sleep on a condition variable until a kernel arrives.
    snicholls::mutex_task_pool pool_;
    std::atomic<bool> stopping_{false};
    std::thread acceptor_;
    std::mutex conns_mutex_;
    std::vector<std::shared_ptr<connection>> conns_;
    std::mutex sessions_mutex_;
    std::map<std::uint64_t, std::shared_ptr<session>> sessions_;
    std::unique_ptr<beacon> beacon_;
};

// =========================================================================== node

/// How a client watches one machine.
struct link_options {
    std::chrono::milliseconds connect_timeout = 3s;
    std::chrono::milliseconds ping_every = 1s;            ///< 0 disables the heartbeat
    std::chrono::milliseconds unresponsive_after = 10s;   ///< no pong this long: drop and reconnect
    std::chrono::milliseconds resume_window = 60s;        ///< then give up on the session's calls
    std::chrono::milliseconds first_retry = 200ms;        ///< reconnect backoff starts here...
    std::chrono::milliseconds max_retry = 10s;            ///< ...and doubles up to here
};

/// One named machine, across however many connections it takes. Calls are
/// pipelined; replies may arrive in any order and are matched by call id.
class node {
public:
    /// Completion of one call: ok with the result bytes, or not ok with the
    /// error code and message. Runs on a node thread.
    using completion = std::function<void(bool ok, detail::err_code code, std::string_view payload_or_error)>;
    using progress_fn = std::function<void(double, std::string_view)>;

    /// Connect and authenticate; throws remote_error if that fails.
    node(std::string key, std::string host, std::uint16_t port, link_options opt = {})
        : key_(std::move(key)), host_(std::move(host)), port_(port), opt_(opt)
    {
        session_ = 0;
        while (session_ == 0) {
            reader r(random_bytes(8));
            session_ = r.get<std::uint64_t>();
        }
        std::string why;
        auto c = connect_once(why);
        if (!c)
            throw remote_error(why);
        attach(std::move(c), {});
        supervisor_ = std::thread([this] { supervise(); });
    }

    node(const node&) = delete;
    node& operator=(const node&) = delete;

    ~node()
    {
        {
            std::lock_guard lk(m_);
            stopping_ = true;
        }
        cv_.notify_all();
        if (supervisor_.joinable())
            supervisor_.join();
        std::shared_ptr<detail::wire_conn> c;
        std::thread r;
        {
            std::lock_guard lk(m_);
            c = conn_;
            r = std::move(reader_);
        }
        if (c)
            c->stream.shutdown();
        if (r.joinable())
            r.join();
        fail_all("the cluster was destroyed with the call outstanding");
    }

    const node_info& info() const noexcept { return info_; }

    /// Connected and authenticated right now.
    bool connected() const
    {
        std::lock_guard lk(m_);
        return conn_ != nullptr;
    }

    /// Connected, or away for less than the resume window.
    bool reachable() const
    {
        std::lock_guard lk(m_);
        return conn_ != nullptr || (!gave_up_ && steady::now() - lost_at_ < opt_.resume_window);
    }

    std::size_t reconnects() const
    {
        std::lock_guard lk(m_);
        return reconnects_;
    }

    /// The last health report and its round trip, from the heartbeat.
    std::optional<std::pair<node_health, steady::duration>> last_health() const
    {
        std::lock_guard lk(m_);
        if (!have_health_)
            return std::nullopt;
        return std::pair{health_, rtt_};
    }

    /// Ping now and wait for the answer. nullopt on timeout or while disconnected.
    std::optional<std::pair<node_health, steady::duration>> ping(std::chrono::milliseconds timeout = 2s)
    {
        std::unique_lock lk(m_);
        if (!conn_)
            return std::nullopt;
        const std::uint64_t nonce = ++ping_nonce_;
        auto c = conn_;
        lk.unlock();
        const auto t0 = steady::now();
        send_ping(*c, nonce);
        lk.lock();
        if (!cv_.wait_for(lk, timeout, [&] { return pong_nonce_ >= nonce || stopping_; }) || pong_nonce_ < nonce)
            return std::nullopt;
        return std::pair{health_, steady::now() - t0};
    }

    /// Send one call; returns its id. While reconnecting the call is held and
    /// sent on reconnect; after giving up it fails at once.
    std::uint64_t call(std::string_view kernel, std::uint64_t sig, std::string_view args, completion complete,
                       progress_fn on_progress = {})
    {
        std::string payload;
        std::shared_ptr<detail::wire_conn> c;
        std::uint64_t id = 0;
        std::string frame;
        {
            std::lock_guard lk(m_);
            if (!conn_ && (gave_up_ || stopping_)) {
                const auto why = "machine '" + info_.name + "' is not reachable: " + lost_reason_;
                complete(false, detail::err_code::failed, why);
                return 0;
            }
            id = next_id_++;
            encode(payload, id);
            payload.append(args);
            frame = detail::frame_bytes(frame_kind::call, kernel, sig, payload);
            pending_.emplace(id, pending_call{std::move(complete), std::move(on_progress), frame, false});
            c = conn_;
        }
        if (c)
            c->write(frame);   // if this fails the connection is going; the call is resent on resume
        return id;
    }

    /// Ask the worker to cancel a call; sent now, or on reconnect.
    void cancel(std::uint64_t id)
    {
        std::shared_ptr<detail::wire_conn> c;
        {
            std::lock_guard lk(m_);
            const auto it = pending_.find(id);
            if (it == pending_.end())
                return;
            it->second.cancel_requested = true;
            c = conn_;
        }
        if (c)
            send_cancel(*c, id);
    }

private:
    struct pending_call {
        completion done;
        progress_fn progress;
        std::string frame;   // kept to resend if the worker never got it
        bool cancel_requested = false;
    };

    // ------------------------------------------------------------ connecting

    std::shared_ptr<detail::wire_conn> connect_once(std::string& why)
    {
        auto c = std::make_shared<detail::wire_conn>();
        std::string err;
        c->stream = net::tcp_stream::connect(host_, port_, opt_.connect_timeout, &err);
        if (!c->stream.valid()) {
            why = "cannot connect to " + host_ + ":" + std::to_string(port_) + ": " + err;
            return nullptr;
        }
        c->stream.set_nodelay(true);
        try {
            frame f;
            std::size_t used = 0;
            if (!detail::read_frame(c->stream, c->buf, f, used, opt_.connect_timeout) || f.kind != frame_kind::hello) {
                why = "no hello from " + host_ + ":" + std::to_string(port_);
                return nullptr;
            }
            if (f.signature != detail::protocol_signature) {
                why = "the worker at " + host_ + ":" + std::to_string(port_) + " speaks a different protocol version";
                return nullptr;
            }
            node_info info;
            std::string server_nonce;
            reader hr(f.payload);
            hr.get(info);
            hr.get(server_nonce);
            c->buf.erase(0, used);

            const std::string client_nonce = random_bytes(32);
            std::string auth;
            encode(auth, session_, client_nonce, detail::client_proof(key_, server_nonce, client_nonce, session_));
            c->write(detail::frame_bytes(frame_kind::auth, "client", detail::protocol_signature, auth));

            if (!detail::read_frame(c->stream, c->buf, f, used, opt_.connect_timeout)) {
                why = "no answer to authentication from " + info.name;
                return nullptr;
            }
            if (f.kind == frame_kind::bye) {
                why = reader(f.payload).get<std::string>();
                return nullptr;
            }
            reader wr(f.payload);
            const auto proof = wr.get<std::string>();
            if (f.kind != frame_kind::welcome ||
                !equal_constant_time(proof, detail::server_proof(key_, server_nonce, client_nonce))) {
                why = "the machine at " + host_ + ":" + std::to_string(port_) + " calling itself '" + info.name +
                      "' failed authentication: it does not hold the shared key";
                return nullptr;
            }
            known_after_welcome_.clear();
            wr.get(known_after_welcome_);
            c->buf.erase(0, used);
            if (info_.name.empty())
                info_ = info;
            return c;
        }
        catch (const std::exception& e) {
            why = std::string("handshake with ") + host_ + " failed: " + e.what();
            return nullptr;
        }
    }

    // Make c the connection, resend what the worker never got, start reading
    void attach(std::shared_ptr<detail::wire_conn> c, std::thread old_reader)
    {
        if (old_reader.joinable())
            old_reader.join();
        const std::set<std::uint64_t> known(known_after_welcome_.begin(), known_after_welcome_.end());
        std::vector<std::string> resend;
        std::vector<std::uint64_t> recancel;
        {
            std::lock_guard lk(m_);
            for (const auto& [id, p] : pending_) {
                if (known.count(id) == 0)
                    resend.push_back(p.frame);
                if (p.cancel_requested)
                    recancel.push_back(id);
            }
            conn_ = c;
            gave_up_ = false;
            last_pong_ = steady::now();
            reader_ = std::thread([this, c] { read_loop(c); });
        }
        for (const auto& f : resend)
            c->write(f);
        for (const auto id : recancel)
            send_cancel(*c, id);
        cv_.notify_all();
    }

    // Heartbeat while connected; reconnect with backoff while not
    void supervise()
    {
        auto next_ping = steady::now();
        auto backoff = opt_.first_retry;
        auto next_try = steady::now();
        std::uint64_t outstanding_nonce = 0;
        steady::time_point outstanding_sent{};
        bool was_connected = true;
        std::unique_lock lk(m_);
        while (!stopping_) {
            cv_.wait_for(lk, 50ms);
            if (stopping_)
                break;
            const auto now = steady::now();
            if (conn_) {
                was_connected = true;
                backoff = opt_.first_retry;
                if (outstanding_nonce != 0 && pong_nonce_ >= outstanding_nonce) {
                    rtt_ = last_pong_ - outstanding_sent;
                    outstanding_nonce = 0;
                }
                if (opt_.ping_every.count() > 0 && now >= next_ping) {
                    outstanding_nonce = ++ping_nonce_;
                    outstanding_sent = now;
                    next_ping = now + opt_.ping_every;
                    auto c = conn_;
                    lk.unlock();
                    send_ping(*c, outstanding_nonce);
                    lk.lock();
                    continue;
                }
                if (opt_.ping_every.count() > 0 && now - last_pong_ >= opt_.unresponsive_after) {
                    const auto secs = std::chrono::duration<double>(now - last_pong_).count();
                    lost_reason_ = info_.name + " is unresponsive: no answer to pings for " +
                                   std::to_string(int(secs + 0.5)) + " s";
                    auto c = conn_;
                    lk.unlock();
                    c->stream.shutdown();   // the reader sees the close and detaches
                    lk.lock();
                }
                continue;
            }

            // Disconnected. The first attempt waits first_retry from the drop, so
            // a worker restarting or a link flapping is not hammered at once
            if (was_connected) {
                was_connected = false;
                next_try = lost_at_ + opt_.first_retry;
                backoff = opt_.first_retry;
            }
            if (!gave_up_ && now - lost_at_ >= opt_.resume_window) {
                gave_up_ = true;
                const auto why = lost_reason_ + ", and it did not come back within " +
                                 std::to_string(std::chrono::duration_cast<std::chrono::seconds>(opt_.resume_window).count()) +
                                 " s";
                lk.unlock();
                fail_all(why);
                lk.lock();
            }
            if (now < next_try)
                continue;
            std::thread old = std::move(reader_);
            lk.unlock();
            std::string why;
            auto c = connect_once(why);
            if (c) {
                attach(std::move(c), std::move(old));
                lk.lock();
                ++reconnects_;
                next_ping = steady::now();
                outstanding_nonce = 0;
                continue;
            }
            if (old.joinable())
                old.join();
            lk.lock();
            next_try = steady::now() + backoff;
            backoff = std::min(backoff * 2, opt_.max_retry);
        }
    }

    // ---------------------------------------------------------------- reading

    void read_loop(std::shared_ptr<detail::wire_conn> c)
    {
        frame f;
        std::size_t used = 0;
        std::string reason = "connection to " + info_.name + " lost";
        bool final = false;
        try {
            while (!stopping_) {
                const auto st = detail::read_frame_status(c->stream, c->buf, f, used, 250ms);
                if (st == detail::read_status::closed)
                    break;
                if (st != detail::read_status::frame)
                    continue;
                if (f.kind == frame_kind::bye) {
                    reason = "connection to " + info_.name + " lost: " + reader(f.payload).get<std::string>();
                    final = true;   // the worker ended our session itself: nothing to wait for
                    break;
                }
                deliver(*c, f);
                c->buf.erase(0, used);
            }
        }
        catch (const std::exception& e) {
            reason = std::string("bad frame from ") + info_.name + ": " + e.what();
        }
        c->stream.shutdown();
        {
            std::lock_guard lk(m_);
            if (conn_ == c) {
                conn_ = nullptr;
                lost_at_ = steady::now();
                if (lost_reason_.empty() || lost_reason_.find("unresponsive") == std::string::npos)
                    lost_reason_ = reason;
            }
            if (final)
                gave_up_ = true;
        }
        if (final)
            fail_all(reason);
        cv_.notify_all();
    }

    void deliver(detail::wire_conn& c, const frame& f)
    {
        std::string_view payload = f.payload;
        if (f.kind == frame_kind::pong) {
            reader r(payload);
            const auto nonce = r.get<std::uint64_t>();
            node_health h;
            r.get(h);
            {
                std::lock_guard lk(m_);
                pong_nonce_ = std::max(pong_nonce_, nonce);
                health_ = h;
                have_health_ = true;
                last_pong_ = steady::now();
                if (lost_reason_.find("unresponsive") != std::string::npos)
                    lost_reason_.clear();
            }
            cv_.notify_all();
            return;
        }

        const auto id = detail::take_call_id(payload);
        if (f.kind == frame_kind::prog) {
            progress_fn p;
            {
                std::lock_guard lk(m_);
                if (const auto it = pending_.find(id); it != pending_.end())
                    p = it->second.progress;
            }
            if (p) {
                reader r(payload);
                const auto fraction = r.get<double>();
                p(fraction, r.get<std::string>());
            }
            return;
        }
        if (f.kind != frame_kind::ret && f.kind != frame_kind::err)
            return;

        // Ack first, whether or not we were waiting: a duplicate after a
        // reconnect must still free the worker's copy
        std::string ack;
        encode(ack, id);
        c.write(detail::frame_bytes(frame_kind::ack, "result", 0, ack));

        completion done;
        {
            std::lock_guard lk(m_);
            const auto it = pending_.find(id);
            if (it == pending_.end())
                return;
            done = std::move(it->second.done);
            pending_.erase(it);
        }
        if (f.kind == frame_kind::ret)
            done(true, detail::err_code::failed, payload);
        else {
            reader r(payload);
            const auto code = static_cast<detail::err_code>(r.get<std::uint8_t>());
            done(false, code, r.get<std::string>());
        }
    }

    // ---------------------------------------------------------------- helpers

    static void send_ping(detail::wire_conn& c, std::uint64_t nonce)
    {
        std::string payload;
        encode(payload, nonce);
        c.write(detail::frame_bytes(frame_kind::ping, "health", detail::protocol_signature, payload));
    }

    static void send_cancel(detail::wire_conn& c, std::uint64_t id)
    {
        std::string payload;
        encode(payload, id);
        c.write(detail::frame_bytes(frame_kind::cancel, "job", 0, payload));
    }

    void fail_all(const std::string& why)
    {
        std::map<std::uint64_t, pending_call> orphans;
        {
            std::lock_guard lk(m_);
            orphans.swap(pending_);
        }
        for (auto& [id, call] : orphans)
            call.done(false, detail::err_code::failed, why);
    }

    std::string key_;
    std::string host_;
    std::uint16_t port_;
    link_options opt_;
    std::uint64_t session_ = 0;
    node_info info_;
    std::vector<std::uint64_t> known_after_welcome_;   // only touched by the connecting thread

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::shared_ptr<detail::wire_conn> conn_;
    std::thread reader_;
    std::map<std::uint64_t, pending_call> pending_;
    std::uint64_t next_id_ = 1;
    // Atomic: the reader thread polls it without the lock. Still set under m_
    // so the supervisor's condition-variable wait cannot miss it.
    std::atomic<bool> stopping_{false};
    bool gave_up_ = false;
    steady::time_point lost_at_;
    std::string lost_reason_;
    std::size_t reconnects_ = 0;
    std::uint64_t ping_nonce_ = 0, pong_nonce_ = 0;
    node_health health_;
    bool have_health_ = false;
    steady::duration rtt_{};
    steady::time_point last_pong_ = steady::now();
    std::thread supervisor_;
};

// ====================================================================== placement

/// Run on the machine with this name.
struct on {
    std::string name;
};

/// Run on any connected machine that meets the requirements, round robin.
struct any {
    std::uint32_t min_cores = 0;
    std::string arch;   ///< empty: any architecture
};

/// Run here, in this process, without serialising.
struct local {};

using placement = std::variant<on, any, local>;

// ============================================================================ job

/// A running remote job: its result, its progress, and a way to stop it.
template <typename R>
class job {
public:
    /// The result, as a std::future; get() rethrows remote_error or job_cancelled.
    std::future<R>& result() noexcept { return result_; }
    R get() { return result_.get(); }

    /// The latest progress the kernel reported (fraction 0 and no note until it does).
    job_progress progress() const
    {
        std::lock_guard lk(state_->mutex);
        return state_->latest;
    }

    /// Ask the worker to stop this job. Cooperative: the kernel must check.
    void cancel()
    {
        if (auto n = state_->link.lock())
            n->cancel(state_->id);
    }

    bool done() const { return result_.wait_for(0s) == std::future_status::ready; }
    const std::string& node_name() const noexcept { return state_->where; }
    steady::duration elapsed() const noexcept { return steady::now() - state_->started; }

    struct shared {
        std::mutex mutex;
        job_progress latest;
        std::weak_ptr<node> link;
        std::uint64_t id = 0;
        std::string where;   // the machine's name
        steady::time_point started = steady::now();
    };

    job(std::future<R> f, std::shared_ptr<shared> s) : result_(std::move(f)), state_(std::move(s)) {}

private:
    std::future<R> result_;
    std::shared_ptr<shared> state_;
};

// ================================================================== remote_thread

/// std::jthread's shape for a remote kernel: joins in its destructor.
class remote_thread {
public:
    remote_thread() = default;
    template <typename R>
    explicit remote_thread(std::future<R> f)
        : wait_([fut = std::make_shared<std::future<R>>(std::move(f))] { fut->get(); })
    {
    }
    remote_thread(remote_thread&&) noexcept = default;
    remote_thread& operator=(remote_thread&& o) noexcept
    {
        if (this != &o) {
            if (joinable())
                quiet_join();
            wait_ = std::move(o.wait_);
        }
        return *this;
    }
    ~remote_thread()
    {
        if (joinable())
            quiet_join();
    }

    bool joinable() const noexcept { return static_cast<bool>(wait_); }

    /// Wait for the kernel; rethrows remote_error if it failed.
    void join()
    {
        auto w = std::move(wait_);
        wait_ = nullptr;
        if (w)
            w();
    }

private:
    // A destructor must not throw; a failure is only reported to join()
    void quiet_join() noexcept
    {
        try {
            join();
        }
        catch (...) {
        }
    }

    std::function<void()> wait_;
};

/// One machine's state, as cluster::health() reports it.
struct node_status {
    node_info info;
    bool connected = false;
    bool reachable = false;              ///< connected, or within the resume window
    std::size_t reconnects = 0;
    std::optional<node_health> health;   ///< from the latest pong
    steady::duration rtt{};              ///< that pong's round trip
};

// ======================================================================== cluster

/// The machines that can run the kernels in Set, by name.
template <typename Set>
class cluster {
public:
    explicit cluster(std::string key) : key_(std::move(key))
    {
        if (key_.size() < 16)
            throw remote_error("cluster: the shared key is missing or too short");
    }

    ~cluster() { listener_.reset(); }   // stop discovery before the nodes go

    /// Connect to a worker and authenticate. Its hello must carry the same name.
    void add(const std::string& name, const std::string& host, std::uint16_t port, link_options opt = {})
    {
        auto n = std::make_shared<node>(key_, host, port, opt);
        if (n->info().name != name)
            throw remote_error("asked for '" + name + "' at " + host + ":" + std::to_string(port) +
                               " but the worker there is called '" + n->info().name + "'");
        std::lock_guard lk(mutex_);
        nodes_[name] = std::move(n);
    }

    /// Listen for workers announcing themselves with this cluster's key and add
    /// each one the first time it is heard. `found` is told about each addition.
    void discover(discovery_options opt = {}, std::function<void(const node_info&)> found = {},
                  link_options link = {})
    {
        listener_ = std::make_unique<beacon_listener>(
            key_,
            [this, found = std::move(found), link](const advertised& a) {
                if (a.protocol != detail::protocol_signature)
                    return;
                {
                    std::lock_guard lk(mutex_);
                    if (nodes_.count(a.name) != 0)
                        return;
                }
                try {
                    add(a.name, a.address, a.port, link);
                    if (found)
                        found(nodes_.at(a.name)->info());
                }
                catch (const std::exception&) {
                    // Heard but not reachable yet; the next beacon tries again
                }
            },
            std::move(opt));
    }

    /// Machines connected right now.
    std::vector<node_info> nodes() const
    {
        std::lock_guard lk(mutex_);
        std::vector<node_info> out;
        for (const auto& [n, link] : nodes_)
            if (link->connected())
                out.push_back(link->info());
        return out;
    }

    /// Every machine's state from its latest heartbeat. Never blocks on the network.
    std::vector<node_status> health() const
    {
        std::lock_guard lk(mutex_);
        std::vector<node_status> out;
        for (const auto& [n, link] : nodes_) {
            node_status s{link->info(), link->connected(), link->reachable(), link->reconnects(), std::nullopt, {}};
            if (auto h = link->last_health()) {
                s.health = h->first;
                s.rtt = h->second;
            }
            out.push_back(std::move(s));
        }
        return out;
    }

    /// Ping one machine now and wait for its answer.
    std::optional<std::pair<node_health, steady::duration>> ping(const std::string& name,
                                                                 std::chrono::milliseconds timeout = 2s)
    {
        return find(name)->ping(timeout);
    }

    /// Start fn(args...) where `where` says and return a handle to watch it by.
    template <typename R, typename... P, typename... A>
    job<R> submit(placement where, R (*fn)(P...), A&&... args)
    {
        static_assert(sizeof...(P) == sizeof...(A), "wrong number of arguments for this kernel");
        auto state = std::make_shared<typename job<R>::shared>();
        if (std::holds_alternative<local>(where)) {
            state->where = "local";
            return job<R>(std::async(std::launch::async, fn, std::forward<A>(args)...), state);
        }

        const int k = Set::index_of(fn);
        if (k < 0)
            throw remote_error("not a kernel in this cluster's kernel_set");

        // Convert to the parameter types first, so what crosses the wire is
        // exactly what the kernel's signature says, whatever the caller passed
        std::string payload;
        {
            const std::tuple<std::remove_cvref_t<P>...> values(std::forward<A>(args)...);
            std::apply([&payload](const auto&... v) { encode(payload, v...); }, values);
        }

        auto promise = std::make_shared<std::promise<R>>();
        auto future = promise->get_future();
        auto target = pick(where);
        state->link = target;
        state->where = target->info().name;

        const auto on_done = [promise](bool ok, detail::err_code code, std::string_view bytes) {
            if (!ok) {
                const std::string why(bytes);
                promise->set_exception(code == detail::err_code::cancelled
                                           ? std::make_exception_ptr(job_cancelled(why))
                                           : std::make_exception_ptr(remote_error(why)));
                return;
            }
            try {
                reader r(bytes);
                if constexpr (std::is_void_v<R>) {
                    r.expect_end();
                    promise->set_value();
                }
                else {
                    R value{};
                    r.get(value);
                    r.expect_end();
                    promise->set_value(std::move(value));
                }
            }
            catch (const std::exception& e) {
                promise->set_exception(std::make_exception_ptr(remote_error(std::string("bad reply: ") + e.what())));
            }
        };
        const auto on_progress = [weak = std::weak_ptr(state)](double fraction, std::string_view note) {
            if (auto s = weak.lock()) {
                std::lock_guard lk(s->mutex);
                s->latest = job_progress{fraction, std::string(note), steady::now()};
            }
        };
        state->id = target->call(Set::names[k], Set::signatures[k], payload, on_done, on_progress);
        return job<R>(std::move(future), std::move(state));
    }

    /// Run fn(args...) where `where` says, and return its result as a std::future.
    template <typename R, typename... P, typename... A>
    std::future<R> async(placement where, R (*fn)(P...), A&&... args)
    {
        return std::move(submit(std::move(where), fn, std::forward<A>(args)...).result());
    }

    /// Like std::invoke: call fn(args...) where `where` says, wait, and return
    /// its result. Throws remote_error (or job_cancelled) where std::invoke
    /// would propagate the callee's exception.
    template <typename R, typename... P, typename... A>
    R invoke(placement where, R (*fn)(P...), A&&... args)
    {
        return async(std::move(where), fn, std::forward<A>(args)...).get();
    }

    /// Like std::jthread: start fn(args...) where `where` says; the handle joins on destruction.
    template <typename R, typename... P, typename... A>
    remote_thread spawn(placement where, R (*fn)(P...), A&&... args)
    {
        return remote_thread(async(std::move(where), fn, std::forward<A>(args)...));
    }

    /// Send a hand-built call, for testing what workers refuse. The future
    /// holds the error message, or "" on success.
    std::future<std::string> call_raw(const std::string& name, std::string_view kernel, std::uint64_t sig,
                                      std::string_view args)
    {
        auto p = std::make_shared<std::promise<std::string>>();
        auto f = p->get_future();
        find(name)->call(kernel, sig, args, [p](bool ok, detail::err_code, std::string_view bytes) {
            p->set_value(ok ? std::string() : std::string(bytes));
        });
        return f;
    }

private:
    std::shared_ptr<node> find(const std::string& name) const
    {
        std::lock_guard lk(mutex_);
        const auto it = nodes_.find(name);
        if (it == nodes_.end())
            throw remote_error("no machine called '" + name + "' in this cluster");
        return it->second;
    }

    std::shared_ptr<node> pick(const placement& where)
    {
        if (const auto* n = std::get_if<on>(&where)) {
            auto target = find(n->name);
            if (!target->reachable())
                throw remote_error("machine '" + n->name + "' is not reachable");
            return target;   // if it is reconnecting, the call waits for it
        }
        std::lock_guard lk(mutex_);
        const auto& req = std::get<any>(where);
        std::vector<std::shared_ptr<node>> fit;
        for (auto& [name, n] : nodes_)
            if (n->connected() && n->info().cores >= req.min_cores && (req.arch.empty() || n->info().arch == req.arch))
                fit.push_back(n);
        if (fit.empty())
            throw remote_error("no connected machine meets the placement requirements");
        return fit[next_++ % fit.size()];
    }

    std::string key_;
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<node>> nodes_;
    std::size_t next_ = 0;
    std::unique_ptr<beacon_listener> listener_;
};

} // namespace snicholls::remote

#endif /* remote_cluster_hpp */
