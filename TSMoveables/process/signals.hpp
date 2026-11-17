//
//  signals.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - POSIX signals turned into ordinary calls, where
//  code may lock, allocate and log, which a signal handler may not:
//
//      snicholls::signal_watcher w([&](int sig) {        // on a thread of its own
//          if (sig == SIGHUP) reload(); else stop();
//      });
//
//      snicholls::signal_watcher w(loop, [&](int sig) {  // or as a task on an event loop
//          loop.stop();
//      });
//
//  The handler does one thing: write the signal's number into a pipe (saving
//  and restoring errno). The watcher thread, or the event loop watching the
//  pipe, reads it and calls you. That is the self-pipe trick, and it is the
//  only way to do real work in response to a signal without undefined
//  behaviour - hand-written handlers seen in practice call
//  stop() or logged from inside the handler, which can deadlock on any lock
//  the interrupted code held.
//
//    - A second SIGINT or SIGTERM exits at once with 128 + n
//      (force_exit_on_repeat), so a hung shutdown can always be interrupted.
//    - The previous dispositions are restored when the watcher is destroyed.
//      One watcher may be active at a time; a second throws.
//    - SIGPIPE is ignored while the watcher lives (ignore_sigpipe), so a
//      write to a closed socket is EPIPE rather than death.
//      scoped_ignore_sigpipe does just that, for code with no watcher.
//    - A callback that throws is reported on stderr; the watcher carries on.
//
//  install_crash_handlers() reports SIGSEGV, SIGBUS, SIGILL, SIGFPE and
//  SIGABRT on stderr from an alternate stack (so a stack overflow is reported
//  too), calls the crash hook if one is set, and re-raises, so the core dump
//  and the exit status are what they would have been. The hook runs inside a
//  signal handler: it must be async-signal-safe (write(), not printf). The
//  alternate stack covers the thread that installs the handlers.
//
//  Ported from an earlier application framework's signals.hpp; the event-loop form, scoped_ignore_sigpipe
//  and set_crash_hook are new here. POSIX only: on Windows this compiles to
//  nothing and SNICHOLLS_HAS_SIGNALS is 0.
//

#ifndef ts_moveables_process_signals_hpp
#define ts_moveables_process_signals_hpp

#include "../event/loop.hpp"

#if defined(_WIN32)
#define SNICHOLLS_HAS_SIGNALS 0
#else
#define SNICHOLLS_HAS_SIGNALS 1

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include "exit_code.hpp"

namespace snicholls {

[[nodiscard]] inline std::string signal_name(int sig)
{
    switch (sig) {
    case SIGINT: return "SIGINT";
    case SIGTERM: return "SIGTERM";
    case SIGHUP: return "SIGHUP";
    case SIGQUIT: return "SIGQUIT";
    case SIGUSR1: return "SIGUSR1";
    case SIGUSR2: return "SIGUSR2";
    case SIGPIPE: return "SIGPIPE";
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    default: return "signal " + std::to_string(sig);
    }
}

// Called from the crash handler, inside the signal: async-signal-safe code only
using crash_hook = void (*)(int signal_number) noexcept;

namespace detail::sig {

static_assert(std::atomic<int>::is_always_lock_free, "snicholls: signal handlers need a lock-free atomic<int>");
#if defined(NSIG)
// The pipe carries a signal's number in one byte, 0 meaning "stop"
static_assert(NSIG < 256, "snicholls: signal numbers must fit in a byte");
#endif

struct globals
{
    std::atomic<int> write_fd{-1};
    std::atomic<int> terminate_signals{0};   // SIGINT/SIGTERM received while a watcher is active
    std::atomic<bool> force_exit_on_repeat{true};
    std::atomic<bool> active{false};
    std::atomic<crash_hook> hook{nullptr};
};

inline globals& state() noexcept
{
    static globals g;   // trivially destructible, constant-initialised
    return g;
}

inline void write_stderr(std::string_view s) noexcept
{
    while (!s.empty()) {
        const ::ssize_t n = ::write(2, s.data(), s.size());
        if (n > 0) s.remove_prefix(static_cast<std::size_t>(n));
        else if (n < 0 && errno == EINTR) continue;
        else return;
    }
}

// Async-signal-safe integer formatting
inline std::size_t format_int(char* out, int value) noexcept
{
    char digits[16];
    std::size_t n = 0;
    unsigned v = value < 0 ? 0u - static_cast<unsigned>(value) : static_cast<unsigned>(value);
    do {
        digits[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v != 0 && n < sizeof digits);
    std::size_t at = 0;
    if (value < 0) out[at++] = '-';
    while (n > 0) out[at++] = digits[--n];
    return at;
}

extern "C" inline void snicholls_watch_handler(int sig)
{
    const int saved_errno = errno;
    globals& g = state();
    if (sig == SIGINT || sig == SIGTERM) {
        if (g.terminate_signals.fetch_add(1, std::memory_order_acq_rel) >= 1 &&
            g.force_exit_on_repeat.load(std::memory_order_acquire)) {
            char note[96] = "snicholls: second termination signal, exiting now with status ";
            std::size_t len = std::strlen(note);
            len += format_int(note + len, signal_exit_code(sig));
            note[len++] = '\n';
            write_stderr(std::string_view(note, len));
            ::_exit(signal_exit_code(sig));
        }
    }
    const int fd = g.write_fd.load(std::memory_order_acquire);
    if (fd >= 0) {
        const auto byte = static_cast<unsigned char>(sig);
        [[maybe_unused]] const ::ssize_t n = ::write(fd, &byte, 1);   // pipe full: a signal is already pending
    }
    errno = saved_errno;
}

} // namespace detail::sig

struct signal_watcher_options
{
    std::vector<int> signals{SIGINT, SIGTERM, SIGHUP, SIGUSR1, SIGUSR2};
    bool ignore_sigpipe = true;
    bool force_exit_on_repeat = true;
};

class signal_watcher
{
public:
    using callback = std::function<void(int signal_number)>;

    // Deliver on a thread of the watcher's own
    explicit signal_watcher(callback on_signal, signal_watcher_options options = {})
        : on_signal_(std::move(on_signal)), options_(std::move(options))
    {
        start([this] { thread_ = std::thread([this] { watch_thread(); }); });
    }

#if SNICHOLLS_HAS_EVENT_LOOP
    // Deliver as a task on `loop`. Construct and destroy it on the loop's thread, or before run()
    signal_watcher(event_loop& loop, callback on_signal, signal_watcher_options options = {})
        : on_signal_(std::move(on_signal)), options_(std::move(options))
    {
        start([this, &loop] {
            ::fcntl(pipe_[0], F_SETFL, ::fcntl(pipe_[0], F_GETFL) | O_NONBLOCK);
            watch_ = std::make_unique<event_loop::fd_watch>(loop.watch(pipe_[0]));
            watch_->on_readable([this] { drain_nonblocking(); });
        });
    }
#endif

    signal_watcher(const signal_watcher&) = delete;
    signal_watcher& operator=(const signal_watcher&) = delete;

    ~signal_watcher()
    {
        restore();   // no new deliveries to our handler
        detail::sig::state().write_fd.store(-1, std::memory_order_release);
        if (thread_.joinable()) {
            const unsigned char stop = 0;
            [[maybe_unused]] const ::ssize_t n = ::write(pipe_[1], &stop, 1);
            thread_.join();
        }
#if SNICHOLLS_HAS_EVENT_LOOP
        watch_.reset();
#endif
        close_pipe();
        detail::sig::state().active.store(false, std::memory_order_release);
    }

    // Signals delivered to the callback so far
    [[nodiscard]] int handled() const noexcept { return handled_.load(std::memory_order_acquire); }

    // Forget earlier SIGINT/SIGTERMs, so the next one is a "first" again (after a handled reload, say)
    static void reset_termination_count() noexcept
    {
        detail::sig::state().terminate_signals.store(0, std::memory_order_release);
    }

private:
    template <class Begin>
    void start(Begin begin)
    {
        detail::sig::globals& g = detail::sig::state();
        bool expected = false;
        if (!g.active.compare_exchange_strong(expected, true))
            throw std::logic_error("snicholls: only one signal_watcher may be active");
        try {
            open_pipe();
            g.terminate_signals.store(0, std::memory_order_release);
            g.force_exit_on_repeat.store(options_.force_exit_on_repeat, std::memory_order_release);
            begin();
            g.write_fd.store(pipe_[1], std::memory_order_release);
            install();
        } catch (...) {
            restore();
            g.write_fd.store(-1, std::memory_order_release);
            if (thread_.joinable()) {
                const unsigned char stop = 0;
                [[maybe_unused]] const ::ssize_t n = ::write(pipe_[1], &stop, 1);
                thread_.join();
            }
#if SNICHOLLS_HAS_EVENT_LOOP
            watch_.reset();
#endif
            close_pipe();
            g.active.store(false, std::memory_order_release);
            throw;
        }
    }

    void open_pipe()
    {
        if (::pipe(pipe_) != 0)
            throw std::runtime_error(std::string("snicholls: pipe() failed: ") + std::strerror(errno));
        for (const int fd : pipe_) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        ::fcntl(pipe_[1], F_SETFL, ::fcntl(pipe_[1], F_GETFL) | O_NONBLOCK);
    }

    void close_pipe() noexcept
    {
        for (int& fd : pipe_)
            if (fd >= 0) {
                ::close(fd);
                fd = -1;
            }
    }

    void install()
    {
        struct sigaction action {};
        action.sa_handler = &detail::sig::snicholls_watch_handler;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESTART;
        for (const int sig : options_.signals) {
            struct sigaction previous {};
            if (::sigaction(sig, &action, &previous) != 0)
                throw std::runtime_error("snicholls: sigaction failed for " + signal_name(sig));
            saved_.push_back({sig, previous});
        }
        if (options_.ignore_sigpipe) {
            struct sigaction ignore {};
            ignore.sa_handler = SIG_IGN;
            sigemptyset(&ignore.sa_mask);
            struct sigaction previous {};
            if (::sigaction(SIGPIPE, &ignore, &previous) == 0) saved_.push_back({SIGPIPE, previous});
        }
    }

    void restore() noexcept
    {
        for (auto it = saved_.rbegin(); it != saved_.rend(); ++it) ::sigaction(it->sig, &it->previous, nullptr);
        saved_.clear();
    }

    // Returns false on the stop byte
    bool deliver(const unsigned char* buffer, ::ssize_t n)
    {
        for (::ssize_t i = 0; i < n; ++i) {
            if (buffer[i] == 0) return false;   // the destructor
            handled_.fetch_add(1, std::memory_order_acq_rel);
            const int sig = buffer[i];
            try {
                on_signal_(sig);
            } catch (const std::exception& e) {
                detail::sig::write_stderr("snicholls: the callback for " + signal_name(sig) + " threw: " +
                                          e.what() + "\n");
            } catch (...) {
                detail::sig::write_stderr("snicholls: the callback for " + signal_name(sig) + " threw\n");
            }
        }
        return true;
    }

    void watch_thread()
    {
        for (;;) {
            unsigned char buffer[64];
            const ::ssize_t n = ::read(pipe_[0], buffer, sizeof buffer);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0 || !deliver(buffer, n)) return;
        }
    }

    void drain_nonblocking()
    {
        for (;;) {
            unsigned char buffer[64];
            const ::ssize_t n = ::read(pipe_[0], buffer, sizeof buffer);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0 || !deliver(buffer, n)) return;
        }
    }

    struct saved_action
    {
        int sig;
        struct sigaction previous;
    };

    callback on_signal_;
    signal_watcher_options options_;
    int pipe_[2]{-1, -1};
    std::vector<saved_action> saved_;
    std::atomic<int> handled_{0};
    std::thread thread_;
#if SNICHOLLS_HAS_EVENT_LOOP
    std::unique_ptr<event_loop::fd_watch> watch_;
#endif
};

// SIGPIPE ignored for a scope, the previous disposition restored after it
class scoped_ignore_sigpipe
{
public:
    scoped_ignore_sigpipe() noexcept
    {
        struct sigaction ignore {};
        ignore.sa_handler = SIG_IGN;
        sigemptyset(&ignore.sa_mask);
        saved_ = ::sigaction(SIGPIPE, &ignore, &previous_) == 0;
    }
    scoped_ignore_sigpipe(const scoped_ignore_sigpipe&) = delete;
    scoped_ignore_sigpipe& operator=(const scoped_ignore_sigpipe&) = delete;
    ~scoped_ignore_sigpipe()
    {
        if (saved_) ::sigaction(SIGPIPE, &previous_, nullptr);
    }

private:
    struct sigaction previous_ {};
    bool saved_ = false;
};

// ------------------------------------------------------------------ crash handlers

// Set (or clear) the hook the crash handler calls before re-raising; returns the previous one
inline crash_hook set_crash_hook(crash_hook hook) noexcept
{
    return detail::sig::state().hook.exchange(hook, std::memory_order_acq_rel);
}

namespace detail::sig {

extern "C" inline void snicholls_crash_handler(int sig)
{
    char note[128] = "snicholls: fatal signal ";
    std::size_t len = std::strlen(note);
    const char* name = sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : sig == SIGILL ? "SIGILL"
                     : sig == SIGFPE  ? "SIGFPE"  : sig == SIGABRT ? "SIGABRT" : "?";
    for (const char* c = name; *c != '\0'; ++c) note[len++] = *c;
    note[len++] = ' ';
    note[len++] = '(';
    len += format_int(note + len, sig);
    note[len++] = ')';
    note[len++] = '\n';
    write_stderr(std::string_view(note, len));
    if (const crash_hook hook = state().hook.load(std::memory_order_acquire)) hook(sig);
    // SA_RESETHAND put back the default action: re-raise for the core dump and the signal's exit status
    ::raise(sig);
}

inline void terminate_handler() noexcept
{
    static std::atomic<bool> entered{false};
    if (!entered.exchange(true)) {
        write_stderr("snicholls: std::terminate called");
        if (const std::exception_ptr e = std::current_exception()) {
            try {
                std::rethrow_exception(e);
            } catch (const std::exception& ex) {
                write_stderr(": uncaught exception: ");
                write_stderr(ex.what());
            } catch (...) {
                write_stderr(": uncaught non-standard exception");
            }
        }
        write_stderr("\n");
    }
    std::abort();
}

} // namespace detail::sig

// Report fatal signals and std::terminate on stderr, then die as before. Returns false if already installed
inline bool install_crash_handlers()
{
    static std::atomic<bool> installed{false};
    if (installed.exchange(true)) return false;

    static std::array<char, 64 * 1024> alternate_stack;   // so a stack overflow can still be reported
    stack_t stack{};
    stack.ss_sp = alternate_stack.data();
    stack.ss_size = alternate_stack.size();
    stack.ss_flags = 0;
    ::sigaltstack(&stack, nullptr);

    struct sigaction action {};
    action.sa_handler = &detail::sig::snicholls_crash_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESETHAND | SA_ONSTACK;
    for (const int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) ::sigaction(sig, &action, nullptr);

    std::set_terminate(&detail::sig::terminate_handler);
    return true;
}

} // namespace snicholls

#endif // !_WIN32

#endif // ts_moveables_process_signals_hpp
