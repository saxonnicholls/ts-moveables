//
//  net/tcp.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - a plain TCP byte stream, with timeouts
//
//      auto s = net::tcp_stream::connect("127.0.0.1", 9000, 2s);
//      s.write_all(data, n, 1s);
//      const auto got = s.read(buf, sizeof buf, 1s);
//
//      auto l = net::tcp_listener::bind("127.0.0.1", 0, 128);
//      auto peer = l.accept(5s);
//
//  The library had a reactor, an HTTP server and a WebSocket client, and no way
//  to just move bytes over TCP. Every consumer that wanted the fastest possible
//  transport had to write its own sockets - one of them shipped ~250 lines of
//  POSIX as a documented stopgap - so this is the missing floor beneath the
//  protocols rather than a new feature above them.
//
//  --------------------------------------------------------- blocking, on purpose
//
//  These are blocking calls WITH DEADLINES, which is a deliberate choice and not
//  a simpler-than-the-reactor compromise. Request/response code, test harnesses,
//  tools and anything with its own thread are clearer written straight through,
//  and "blocking" only becomes a problem when it can block FOREVER. Every call
//  here takes a timeout and every one of them means it: the deadline is computed
//  once and respected across EINTR and partial transfers, rather than being
//  restarted by each retry, which is the usual way a 1-second timeout quietly
//  becomes unbounded.
//
//  For many connections on one thread there is tcp_connection and tcp_acceptor
//  below: the same bytes, driven by the reactor instead of by your thread, with
//  a bounded write queue and a drain signal so a producer can be told to wait
//  rather than growing memory. Reach for those when the count of connections is
//  the thing that matters; reach for tcp_stream when the clarity of straight-
//  line code is.
//
//  ------------------------------------------------------------ TLS is one line
//
//  A transport_delegate plugs in, so TLS over a raw stream is the same two-axis
//  split the server uses - the stream moves bytes, the delegate transforms them,
//  and neither knows what the other is:
//
//      http::openssl_client_context ctx;               // verifies by default
//      auto s = net::tcp_stream::connect(host, 443, 2s);
//      s.set_transport(ctx.connect(host));             // the one line
//      s.handshake(5s);                    // drives the delegate to established()
//      s.write_all(req.data(), req.size(), 2s);
//
//  ----------------------------------------------------- closing from elsewhere
//
//  close() is for the owning thread. To interrupt a read or write blocked in
//  another thread, call shutdown() - that is the only operation here that is
//  safe to use concurrently with the others, and the distinction is load-bearing
//  rather than pedantic.
//
//  Closing an fd that another thread is about to use is a use-after-free with
//  extra steps: the number is immediately reusable, so the racing read can end
//  up pointed at whatever the next accept() or open() returned, silently reading
//  somebody else's socket. shutdown() cannot do that - it leaves the descriptor
//  in place and merely tears down the connection under it, so the blocked call
//  returns 0 or an error and the owner closes when it is ready.
//

#ifndef net_tcp_hpp
#define net_tcp_hpp

#include <cstddef>
#include <cstdint>
#include <string>

#if defined(_WIN32) || !defined(__has_include)
#define SNICHOLLS_HAS_TCP 0
#else
#define SNICHOLLS_HAS_TCP 1
#endif

#if SNICHOLLS_HAS_TCP

#include "../event/loop.hpp"
#include "../interfaces/transport_delegate.hpp"

#include <chrono>
#include <algorithm>
#include <functional>
#include <memory>
#include <vector>
#include <stdexcept>
#include <utility>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace snicholls {
namespace net {

namespace detail {

using clock = std::chrono::steady_clock;

// A deadline, not a duration. Every wait in this file is derived from one, so a
// timeout spent half-way through a partial write is not handed out again to the
// next attempt - the mistake that turns a bounded call into an unbounded one.
struct deadline {
    clock::time_point at;

    explicit deadline(std::chrono::milliseconds d) : at(clock::now() + d) {}

    int remaining_ms() const
    {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(at - clock::now());
        if (left.count() <= 0)
            return 0;
        const auto ms = left.count();
        return ms > 0x7fffffff ? 0x7fffffff : static_cast<int>(ms);
    }

    bool expired() const { return clock::now() >= at; }
};

// Wait for one event on fd, honouring the deadline. Returns 1 ready, 0 timeout,
// -1 error. EINTR is retried against the SAME deadline rather than restarting
// it.
inline int wait_for(int fd, short events, const deadline& dl)
{
    for (;;) {
        pollfd p{};
        p.fd = fd;
        p.events = events;
        const int rc = ::poll(&p, 1, dl.remaining_ms());
        if (rc < 0 && errno == EINTR) {
            if (dl.expired())
                return 0;
            continue;
        }
        return rc;
    }
}

inline void set_nonblocking(int fd, bool on)
{
    int fl = ::fcntl(fd, F_GETFL, 0);
    if (fl < 0)
        return;
    fl = on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
    ::fcntl(fd, F_SETFL, fl);
}

inline void set_err(std::string* err, const char* what)
{
    if (err)
        *err = std::string(what) + ": " + std::strerror(errno);
}

} // namespace detail

// ------------------------------------------------------------------- the stream
//
// Owns one connected socket. Movable (it is a handle over a descriptor), never
// copyable - two owners closing one fd is the bug this type exists to prevent.
class tcp_stream
{
public:
    tcp_stream() = default;

    ~tcp_stream() { close(); }

    tcp_stream(tcp_stream&& o) noexcept
        : fd_(o.fd_), transport_(std::move(o.transport_)),
          app_in_(std::move(o.app_in_)), wire_out_(std::move(o.wire_out_))
    {
        o.fd_ = -1;
    }

    tcp_stream& operator=(tcp_stream&& o) noexcept
    {
        if (this != &o) {
            close();
            fd_ = o.fd_;
            o.fd_ = -1;
            transport_ = std::move(o.transport_);
            app_in_ = std::move(o.app_in_);
            wire_out_ = std::move(o.wire_out_);
        }
        return *this;
    }

    tcp_stream(const tcp_stream&) = delete;
    tcp_stream& operator=(const tcp_stream&) = delete;

    // Adopt an already-connected descriptor (what accept() hands back).
    explicit tcp_stream(int fd) : fd_(fd) {}

    // Connect, or return an invalid stream. Resolves the host, then tries each
    // address in turn against the one deadline - so a host with a dead AAAA and
    // a live A still connects, and the timeout covers the whole attempt rather
    // than being granted afresh per address.
    static tcp_stream connect(const std::string& host, std::uint16_t port,
                              std::chrono::milliseconds timeout,
                              std::string* err = nullptr)
    {
        const detail::deadline dl(timeout);

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        const std::string service = std::to_string(port);
        const int gai = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &res);
        if (gai != 0) {
            if (err)
                *err = std::string("getaddrinfo: ") + ::gai_strerror(gai);
            return tcp_stream{};
        }

        tcp_stream out;
        for (addrinfo* a = res; a != nullptr; a = a->ai_next) {
            if (dl.expired()) {
                if (err)
                    *err = "connect: timed out";
                break;
            }
            const int fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (fd < 0)
                continue;
            no_sigpipe(fd);
            detail::set_nonblocking(fd, true);

            int rc = ::connect(fd, a->ai_addr, a->ai_addrlen);
            if (rc != 0 && errno == EINPROGRESS) {
                const int w = detail::wait_for(fd, POLLOUT, dl);
                if (w <= 0) {
                    if (err)
                        *err = w == 0 ? "connect: timed out" : "connect: poll failed";
                    ::close(fd);
                    continue;
                }
                int se = 0;
                socklen_t sl = sizeof se;
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &se, &sl) != 0 || se != 0) {
                    errno = se ? se : errno;
                    detail::set_err(err, "connect");
                    ::close(fd);
                    continue;
                }
                rc = 0;
            }
            if (rc != 0) {
                detail::set_err(err, "connect");
                ::close(fd);
                continue;
            }
            out.fd_ = fd;
            if (err)
                err->clear();
            break;
        }
        ::freeaddrinfo(res);
        return out;
    }

    bool valid() const noexcept { return fd_ >= 0; }
    int fd() const noexcept { return fd_; }

    void set_nodelay(bool on)
    {
        if (fd_ < 0)
            return;
        const int v = on ? 1 : 0;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &v, sizeof v);
    }

    bool set_buffer_sizes(int send_bytes, int recv_bytes)
    {
        if (fd_ < 0)
            return false;
        bool ok = true;
        if (send_bytes > 0)
            ok &= ::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &send_bytes, sizeof send_bytes) == 0;
        if (recv_bytes > 0)
            ok &= ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &recv_bytes, sizeof recv_bytes) == 0;
        return ok;
    }

    // Optional byte transformer - TLS, or anything else shaped like it. Set it
    // before any traffic, then drive handshake().
    void set_transport(std::unique_ptr<http::transport_delegate> t) { transport_ = std::move(t); }
    bool has_transport() const noexcept { return transport_ != nullptr; }

    // Drive the transport to established(). A no-op with no transport, so
    // calling it unconditionally is fine.
    bool handshake(std::chrono::milliseconds timeout, std::string* err = nullptr)
    {
        if (!transport_)
            return true;
        if (fd_ < 0)
            return false;
        const detail::deadline dl(timeout);

        std::string first;
        if (!transport_->start(first)) {
            if (err)
                *err = "handshake: transport refused to start";
            return false;
        }
        if (!first.empty() && !raw_write_all(first.data(), first.size(), dl, err))
            return false;

        char buf[16384];
        while (!transport_->established()) {
            if (dl.expired()) {
                if (err)
                    *err = "handshake: timed out";
                return false;
            }
            const int w = detail::wait_for(fd_, POLLIN, dl);
            if (w <= 0) {
                if (err)
                    *err = w == 0 ? "handshake: timed out" : "handshake: poll failed";
                return false;
            }
            const ssize_t n = ::recv(fd_, buf, sizeof buf, 0);
            if (n == 0) {
                if (err)
                    *err = "handshake: peer closed";
                return false;
            }
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                detail::set_err(err, "handshake: recv");
                return false;
            }
            wire_out_.clear();
            if (!transport_->wire_in(buf, std::size_t(n), app_in_, wire_out_)) {
                if (err)
                    *err = "handshake: transport rejected the peer";
                return false;
            }
            if (!wire_out_.empty() &&
                !raw_write_all(wire_out_.data(), wire_out_.size(), dl, err))
                return false;
        }
        return true;
    }

    // Read up to n bytes. Returns the count, 0 on a clean peer close, and -1 on
    // error or timeout (errno is ETIMEDOUT for the latter, so the two are
    // distinguishable without a second channel).
    std::ptrdiff_t read(char* data, std::size_t n, std::chrono::milliseconds timeout,
                        std::string* err = nullptr)
    {
        if (fd_ < 0 || n == 0)
            return -1;
        const detail::deadline dl(timeout);

        for (;;) {
            // Anything the transport already decoded comes first: it may have
            // produced several app records from one wire read.
            if (!app_in_.empty()) {
                const std::size_t take = app_in_.size() < n ? app_in_.size() : n;
                std::memcpy(data, app_in_.data(), take);
                app_in_.erase(0, take);
                return static_cast<std::ptrdiff_t>(take);
            }

            const int w = detail::wait_for(fd_, POLLIN, dl);
            if (w == 0) {
                errno = ETIMEDOUT;
                if (err)
                    *err = "read: timed out";
                return -1;
            }
            if (w < 0) {
                detail::set_err(err, "read: poll");
                return -1;
            }

            char buf[65536];
            char* dst = transport_ ? buf : data;
            const std::size_t cap = transport_ ? sizeof buf : n;
            const ssize_t got = ::recv(fd_, dst, cap, 0);
            if (got == 0)
                return 0;                           // clean EOF
            if (got < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                detail::set_err(err, "read: recv");
                return -1;
            }
            if (!transport_)
                return static_cast<std::ptrdiff_t>(got);

            wire_out_.clear();
            if (!transport_->wire_in(buf, std::size_t(got), app_in_, wire_out_)) {
                if (err)
                    *err = "read: transport error";
                return -1;
            }
            if (!wire_out_.empty() &&
                !raw_write_all(wire_out_.data(), wire_out_.size(), dl, err))
                return -1;
            // Loop: the record may have been protocol-only, yielding no app
            // bytes, in which case we wait again rather than reporting EOF.
        }
    }

    // All n bytes, or false. Partial progress does not reset the deadline.
    bool write_all(const char* data, std::size_t n, std::chrono::milliseconds timeout,
                   std::string* err = nullptr)
    {
        if (fd_ < 0)
            return false;
        if (n == 0)
            return true;
        const detail::deadline dl(timeout);

        if (!transport_)
            return raw_write_all(data, n, dl, err);

        wire_out_.clear();
        if (!transport_->app_out(data, n, wire_out_)) {
            if (err)
                *err = "write: transport error";
            return false;
        }
        return raw_write_all(wire_out_.data(), wire_out_.size(), dl, err);
    }

    // Half-close: send FIN, keep reading. The polite way to say "nothing more
    // from me" to a peer that is still talking.
    bool shutdown_write()
    {
        return fd_ >= 0 && ::shutdown(fd_, SHUT_WR) == 0;
    }

    // THE one call that is safe to make while another thread is blocked in
    // read() or write_all(). It tears the connection down under the descriptor
    // without freeing the number, so the blocked call returns and the owner
    // closes when it is ready. See the header note on why close() cannot do
    // this job.
    bool shutdown()
    {
        return fd_ >= 0 && ::shutdown(fd_, SHUT_RDWR) == 0;
    }

    // Owning thread only.
    void close()
    {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        transport_.reset();
        app_in_.clear();
        wire_out_.clear();
    }

private:
    static void no_sigpipe(int fd)
    {
#if defined(SO_NOSIGPIPE)
        const int on = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#else
        (void)fd;       // Linux has no SO_NOSIGPIPE; MSG_NOSIGNAL is used below
#endif
    }

    bool raw_write_all(const char* data, std::size_t n, const detail::deadline& dl,
                       std::string* err)
    {
        std::size_t off = 0;
        while (off < n) {
            const int w = detail::wait_for(fd_, POLLOUT, dl);
            if (w == 0) {
                errno = ETIMEDOUT;
                if (err)
                    *err = "write: timed out";
                return false;
            }
            if (w < 0) {
                detail::set_err(err, "write: poll");
                return false;
            }
#if defined(MSG_NOSIGNAL)
            const ssize_t sent = ::send(fd_, data + off, n - off, MSG_NOSIGNAL);
#else
            const ssize_t sent = ::send(fd_, data + off, n - off, 0);
#endif
            if (sent < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                detail::set_err(err, "write: send");
                return false;
            }
            off += std::size_t(sent);
        }
        return true;
    }

    int                                  fd_ = -1;
    std::unique_ptr<http::transport_delegate>  transport_;
    std::string                          app_in_;    // decoded, not yet handed out
    std::string                          wire_out_;  // scratch for the delegate
};

// ----------------------------------------------------------------- the listener

class tcp_listener
{
public:
    tcp_listener() = default;
    ~tcp_listener() { close(); }

    tcp_listener(tcp_listener&& o) noexcept : fd_(o.fd_), port_(o.port_) { o.fd_ = -1; }
    tcp_listener& operator=(tcp_listener&& o) noexcept
    {
        if (this != &o) {
            close();
            fd_ = o.fd_;
            port_ = o.port_;
            o.fd_ = -1;
        }
        return *this;
    }
    tcp_listener(const tcp_listener&) = delete;
    tcp_listener& operator=(const tcp_listener&) = delete;

    // port 0 means "any", and port() then reports what the kernel chose - which
    // is what a test wants, so two of them never collide.
    static tcp_listener bind(const std::string& host, std::uint16_t port,
                             int backlog = 128, std::string* err = nullptr)
    {
        tcp_listener out;

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;
        addrinfo* res = nullptr;
        const std::string service = std::to_string(port);
        const int gai = ::getaddrinfo(host.empty() ? nullptr : host.c_str(),
                                      service.c_str(), &hints, &res);
        if (gai != 0) {
            if (err)
                *err = std::string("getaddrinfo: ") + ::gai_strerror(gai);
            return out;
        }

        for (addrinfo* a = res; a != nullptr; a = a->ai_next) {
            const int fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (fd < 0)
                continue;
            const int on = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
            if (::bind(fd, a->ai_addr, a->ai_addrlen) != 0 || ::listen(fd, backlog) != 0) {
                detail::set_err(err, "bind");
                ::close(fd);
                continue;
            }
            detail::set_nonblocking(fd, true);
            out.fd_ = fd;
            out.port_ = resolve_port(fd, port);
            if (err)
                err->clear();
            break;
        }
        ::freeaddrinfo(res);
        return out;
    }

    bool valid() const noexcept { return fd_ >= 0; }
    std::uint16_t port() const noexcept { return port_; }
    int fd() const noexcept { return fd_; }

    // An invalid stream on timeout or error; errno is ETIMEDOUT for a timeout.
    tcp_stream accept(std::chrono::milliseconds timeout, std::string* err = nullptr)
    {
        if (fd_ < 0)
            return tcp_stream{};
        const detail::deadline dl(timeout);
        for (;;) {
            const int w = detail::wait_for(fd_, POLLIN, dl);
            if (w == 0) {
                errno = ETIMEDOUT;
                if (err)
                    *err = "accept: timed out";
                return tcp_stream{};
            }
            if (w < 0) {
                detail::set_err(err, "accept: poll");
                return tcp_stream{};
            }
            const int cfd = ::accept(fd_, nullptr, nullptr);
            if (cfd < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                detail::set_err(err, "accept");
                return tcp_stream{};
            }
            detail::set_nonblocking(cfd, true);
            if (err)
                err->clear();
            return tcp_stream(cfd);
        }
    }

    // Safe while another thread is blocked in accept(), for the same reason
    // tcp_stream::shutdown() is: the descriptor stays put.
    bool shutdown() { return fd_ >= 0 && ::shutdown(fd_, SHUT_RDWR) == 0; }

    // Give up ownership of the descriptor without closing it - for handing a
    // bound listener to something that will own it from here (tcp_acceptor).
    int release() noexcept
    {
        const int f = fd_;
        fd_ = -1;
        return f;
    }

    void close()
    {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    static std::uint16_t resolve_port(int fd, std::uint16_t requested)
    {
        if (requested != 0)
            return requested;
        sockaddr_storage ss{};
        socklen_t sl = sizeof ss;
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &sl) != 0)
            return 0;
        if (ss.ss_family == AF_INET)
            return ntohs(reinterpret_cast<sockaddr_in*>(&ss)->sin_port);
        if (ss.ss_family == AF_INET6)
            return ntohs(reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port);
        return 0;
    }

    int           fd_ = -1;
    std::uint16_t port_ = 0;
};

// ===================================================================== reactor
//
// The same bytes, driven by the loop instead of by your thread. For the case
// tcp_stream is the wrong shape for: many connections, one thread, nobody
// blocking.
//
//     net::tcp_acceptor acc(loop, "127.0.0.1", 9000);
//     acc.on_accept([](net::tcp_connection c) {
//         c.on_data([](net::tcp_connection conn, std::string& buf) {
//             conn.send(consume_frames(buf));     // leave a partial frame in buf
//         });
//     });
//
// -------------------------------------------------------------- backpressure
//
// send() is bounded and says so. Above write_high_water it returns false and
// queues nothing, rather than accepting everything and growing until the
// process dies - which is the failure an unbounded client-side queue produces,
// and the one the errata that prompted this file asked to be able to see
// coming. on_drain fires when the queue empties, so a producer can stop on
// false and resume on the signal instead of polling backlog().
//
// ------------------------------------------------------- consuming the buffer
//
// on_data hands over the connection's OWN read buffer by reference, and the
// handler erases what it has consumed. Anything left stays for the next
// readable event, so a half-arrived length-prefixed frame costs nothing to
// handle: look, decide it is short, return. A callback taking (ptr, len)
// instead would force every caller to keep a second buffer for exactly that
// case.
//
// Everything runs on the loop thread except send() and close(), which marshal.

#if SNICHOLLS_HAS_EVENT_LOOP

struct tcp_conn_config {
    std::size_t read_chunk       = 64u * 1024;
    // send() refuses above this. Not a hint: the point is for a producer to be
    // told, while there is still memory left to tell it with.
    std::size_t write_high_water = 1u * 1024 * 1024;
};

class tcp_connection
{
public:
    tcp_connection() = default;

    // Moveable handle over a shared core, like every other connection type
    // here: the handlers it hands out keep the core alive exactly as long as
    // the loop holds them.
    tcp_connection(tcp_connection&&) noexcept = default;
    tcp_connection& operator=(tcp_connection&&) noexcept = default;
    tcp_connection(const tcp_connection&) = default;
    tcp_connection& operator=(const tcp_connection&) = default;

    // One handler each; setting again replaces. These are deliberately not
    // signals - a byte stream has one owner, and fanning raw bytes to several
    // handlers means each of them is guessing what the others consumed.
    void on_data(std::function<void(tcp_connection, std::string&)> f) const
    {
        if (c_) c_->on_data = std::move(f);
    }
    void on_close(std::function<void(tcp_connection)> f) const
    {
        if (c_) c_->on_close = std::move(f);
    }
    void on_drain(std::function<void(tcp_connection)> f) const
    {
        if (c_) c_->on_drain = std::move(f);
    }

    // Queue bytes. Safe from any thread. false means either closed, or the
    // queue is already at write_high_water and nothing was taken - check
    // connected() to tell which.
    bool send(std::string bytes) const
    {
        if (!c_ || !c_->open.load(std::memory_order_acquire))
            return false;
        if (c_->queued.load(std::memory_order_acquire) + bytes.size() > c_->cfg.write_high_water)
            return false;
        c_->queued.fetch_add(bytes.size(), std::memory_order_acq_rel);
        auto core = c_;
        if (core->poster.on_loop_thread()) {
            core->append_and_flush(std::move(bytes));
        } else {
            core->poster.post([core, b = std::move(bytes)]() mutable {
                core->append_and_flush(std::move(b));
            });
        }
        return true;
    }

    std::size_t backlog() const noexcept
    {
        return c_ ? c_->queued.load(std::memory_order_acquire) : 0;
    }
    bool connected() const noexcept
    {
        return c_ && c_->open.load(std::memory_order_acquire);
    }
    int fd() const noexcept { return c_ ? c_->fd : -1; }

    // Safe from any thread.
    void close() const
    {
        if (!c_)
            return;
        auto core = c_;
        if (core->poster.on_loop_thread())
            core->teardown();
        else
            core->poster.post([core] { core->teardown(); });
    }

private:
    friend class tcp_acceptor;

    struct core : std::enable_shared_from_this<core> {
        int                        fd = -1;
        tcp_conn_config            cfg;
        event_loop::poster         poster;
        event_loop::fd_watch       watch;
        std::string                in;
        std::string                out;
        std::size_t                sent = 0;
        std::atomic<std::size_t>   queued{0};
        std::atomic<bool>          open{true};

        std::function<void(tcp_connection, std::string&)> on_data;
        std::function<void(tcp_connection)>              on_close;
        std::function<void(tcp_connection)>              on_drain;

        // An open connection OWNS ITSELF, and that is the difference between an
        // API people can use and a trap.
        //
        // Without it the natural spelling silently does nothing:
        //
        //     acc.on_accept([](tcp_connection c) { c.on_data(...); });
        //
        // c is a handle; it dies when the handler returns, and the watch holds
        // only a weak reference (deliberately - a strong one there would be a
        // cycle that never frees). So the core went with it, the socket closed,
        // and the peer saw a connection that was accepted and then hung up on.
        // Requiring every caller to park the handle somewhere would be a
        // lifetime puzzle set by the library for no reason.
        //
        // Released in teardown(), which every path reaches - EOF, error, or an
        // explicit close - so the cycle is broken deliberately rather than
        // left to leak.
        std::shared_ptr<core> self;

        void append_and_flush(std::string bytes)
        {
            if (!open.load(std::memory_order_acquire))
                return;
            out += bytes;
            flush();
        }

        void flush()
        {
            while (sent < out.size()) {
#if defined(MSG_NOSIGNAL)
                const ssize_t n = ::send(fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
#else
                const ssize_t n = ::send(fd, out.data() + sent, out.size() - sent, 0);
#endif
                if (n > 0) {
                    sent += std::size_t(n);
                    continue;
                }
                if (n < 0 && (errno == EINTR))
                    continue;
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    // Ask to be told when there is room, and keep the rest.
                    want_write(true);
                    compact();
                    return;
                }
                teardown();
                return;
            }
            out.clear();
            sent = 0;
            queued.store(0, std::memory_order_release);
            want_write(false);
            if (on_drain)
                on_drain(handle());
        }

        // Drop what has gone rather than erase-per-write: the same reason
        // websocket_client stopped memmoving its whole buffer per frame.
        void compact()
        {
            if (sent == 0)
                return;
            if (sent * 2 >= out.size()) {
                out.erase(0, sent);
                sent = 0;
            }
            queued.store(out.size() - sent, std::memory_order_release);
        }

        void want_write(bool on)
        {
            if (writing == on || fd < 0)
                return;
            writing = on;
            try {
                watch.set_interest(on ? (fd_interest::read |
                                         fd_interest::write)
                                      : fd_interest::read);
            } catch (...) {
                // The watch is gone; teardown is already under way.
            }
        }

        void readable()
        {
            if (!open.load(std::memory_order_acquire))
                return;
            std::string chunk;
            chunk.resize(cfg.read_chunk);
            for (;;) {
                const ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
                if (n > 0) {
                    in.append(chunk.data(), std::size_t(n));
                    if (on_data)
                        on_data(handle(), in);
                    if (!open.load(std::memory_order_acquire))
                        return;              // the handler closed us
                    if (std::size_t(n) < chunk.size())
                        return;              // drained
                    continue;
                }
                if (n == 0) {
                    teardown();              // clean EOF
                    return;
                }
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return;
                teardown();
                return;
            }
        }

        // Idempotent, and announces exactly once - the same discipline the
        // WebSocket close path needed: a handler told twice that one connection
        // closed will double-free something.
        void teardown()
        {
            if (!open.exchange(false, std::memory_order_acq_rel))
                return;
            auto keep = handle();            // alive for the rest of this call
            self.reset();                    // give up self-ownership
            watch.reset();
            if (fd >= 0) {
                ::close(fd);
                fd = -1;
            }
            out.clear();
            sent = 0;
            queued.store(0, std::memory_order_release);

            // Drop the handlers too. One that captured its own tcp_connection
            // is a cycle, and this is where it gets broken - the same rule the
            // WebSocket state follows when it clears its parked connections.
            on_data = nullptr;
            on_drain = nullptr;
            auto announce = std::move(on_close);
            on_close = nullptr;
            if (announce)
                announce(keep);
        }

        tcp_connection handle()
        {
            tcp_connection c;
            c.c_ = shared_from_this();
            return c;
        }

        bool writing = false;
    };

    static tcp_connection adopt(event_loop& loop, int fd, tcp_conn_config cfg)
    {
        auto core = std::make_shared<struct core>();
        core->fd = fd;
        core->cfg = cfg;
        core->poster = loop.make_poster();
        detail::set_nonblocking(fd, true);
        core->watch = loop.watch(fd, fd_interest::read);

        std::weak_ptr<struct core> weak = core;
        core->watch.on_readable([weak] {
            if (auto c = weak.lock())
                c->readable();
        });
        core->watch.on_writable([weak] {
            if (auto c = weak.lock())
                c->flush();
        });
        core->watch.on_error([weak] {
            if (auto c = weak.lock())
                c->teardown();
        });

        core->self = core;              // open connections keep themselves alive

        tcp_connection out;
        out.c_ = std::move(core);
        return out;
    }

    std::shared_ptr<core> c_;
};

// Accepts on the loop and hands each connection to on_accept.
class tcp_acceptor
{
public:
    tcp_acceptor() = default;

    tcp_acceptor(event_loop& loop, const std::string& host, std::uint16_t port,
                 int backlog = 128, tcp_conn_config conn = tcp_conn_config{})
    {
        std::string err;
        auto l = tcp_listener::bind(host, port, backlog, &err);
        if (!l.valid())
            throw std::runtime_error("tcp_acceptor: " + err);

        s_ = std::make_shared<state>();
        s_->port = l.port();
        s_->fd = l.fd();
        s_->conn = conn;
        s_->loop = &loop;
        l.release();                         // the acceptor owns the fd now

        s_->watch = loop.watch(s_->fd, fd_interest::read);
        std::weak_ptr<state> weak = s_;
        s_->watch.on_readable([weak] {
            if (auto st = weak.lock())
                st->accept_ready();
        });
    }

    std::uint16_t port() const noexcept { return s_ ? s_->port : 0; }

    void on_accept(std::function<void(tcp_connection)> f) const
    {
        if (s_) s_->on_accept = std::move(f);
    }

    // Stops accepting AND closes what was accepted: an acceptor is the owner
    // of its connections, so outliving it would leave them driven by a loop
    // nobody is stopping.
    void close() const
    {
        if (!s_)
            return;
        s_->watch.reset();
        if (s_->fd >= 0) {
            ::close(s_->fd);
            s_->fd = -1;
        }
        for (auto& w : s_->live)
            if (auto c = w.lock())
                c->teardown();
        s_->live.clear();
    }

private:
    struct state {
        int                                 fd = -1;
        std::uint16_t                       port = 0;
        tcp_conn_config                     conn;
        event_loop*                         loop = nullptr;
        event_loop::fd_watch                watch;
        std::function<void(tcp_connection)> on_accept;
        std::vector<std::weak_ptr<tcp_connection::core>> live;

        void prune()
        {
            live.erase(std::remove_if(live.begin(), live.end(),
                                      [](const std::weak_ptr<tcp_connection::core>& w) {
                                          return w.expired();
                                      }),
                       live.end());
        }

        void accept_ready()
        {
            for (;;) {
                const int cfd = ::accept(fd, nullptr, nullptr);
                if (cfd < 0) {
                    if (errno == EINTR)
                        continue;
                    return;                  // EAGAIN: drained, or a real error
                }
                tcp_connection c = tcp_connection::adopt(*loop, cfd, conn);
                // Track it weakly, so closing the acceptor closes what it
                // accepted. Without this a connection nobody holds would
                // outlive the loop that drives it - self-ownership keeps it
                // alive, and only teardown() releases that.
                prune();
                live.push_back(c.c_);
                if (on_accept)
                    on_accept(c);
                else
                    c.close();               // nobody wants it; do not leak the fd
            }
        }
    };

    std::shared_ptr<state> s_;
};

#endif // SNICHOLLS_HAS_EVENT_LOOP

} // namespace net
} // namespace snicholls

#endif // SNICHOLLS_HAS_TCP
#endif /* net_tcp_hpp */
