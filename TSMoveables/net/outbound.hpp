//
//  outbound.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - one outbound connection on an event loop: resolve,
//  connect, optionally TLS, then bytes in and bytes out. The part of a client
//  that is the same for every protocol, so it is written once:
//
//      auto c = std::make_shared<snicholls::net::outbound_connection>(loop, "example.com", "443", tls_factory);
//      c->on_open  = [&] { c->write(request); };
//      c->on_data  = [&](std::string& in) { /* consume from the front of in */ };
//      c->on_close = [&](snicholls::net::outbound_status why) { ... };
//      c->start();
//
//    - Every address the name resolves to is tried in order. A non-blocking
//      connect() to a dead address returns EINPROGRESS exactly like a live
//      one, so "the first that did not fail at once" silently commits to the
//      wrong one; `localhost` resolving to ::1 first while only IPv4 listens
//      is the everyday case.
//    - A transport (TLS) gets every byte both ways when one is given: it
//      speaks first (the ClientHello), and bytes it owes the peer in reply to
//      bytes received are flushed at once, or the handshake stalls.
//    - on_open fires when the transport is established, so the first write
//      already rides inside TLS.
//    - Loop thread only: start(), write() and close() are called there, and
//      the callbacks run there. Own it by shared_ptr; the watch holds only
//      a weak reference, so destroying the owner's pointer ends it.
//
//  Extracted from http::websocket_client's connection core so http::client
//  shares it. POSIX, like the event loop.
//

#ifndef ts_moveables_net_outbound_hpp
#define ts_moveables_net_outbound_hpp

#include "../event/loop.hpp"

#if SNICHOLLS_HAS_EVENT_LOOP

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../interfaces/transport_delegate.hpp"

namespace snicholls {
namespace net {

enum class outbound_status : std::uint8_t {
    closed_by_us,       // close() was called
    closed_by_peer,     // orderly EOF
    resolve_failed,     // the name did not resolve
    connect_failed,     // every address refused or timed out
    transport_error,    // the socket or the TLS engine failed after connecting
    handshake_failed,   // the TLS handshake failed
    timed_out,          // the connect timeout passed
};

inline const char* to_string(outbound_status s) noexcept
{
    switch (s) {
    case outbound_status::closed_by_us: return "closed by us";
    case outbound_status::closed_by_peer: return "closed by peer";
    case outbound_status::resolve_failed: return "name did not resolve";
    case outbound_status::connect_failed: return "connect failed";
    case outbound_status::transport_error: return "transport error";
    case outbound_status::handshake_failed: return "TLS handshake failed";
    case outbound_status::timed_out: return "connect timed out";
    }
    return "?";
}

using transport_factory = std::function<std::unique_ptr<http::transport_delegate>(const std::string& host)>;

class outbound_connection : public std::enable_shared_from_this<outbound_connection> {
public:
    outbound_connection(event_loop& loop, std::string host, std::string port, transport_factory factory = {},
                        std::chrono::milliseconds connect_timeout = std::chrono::seconds{10})
        : loop_(&loop), host_(std::move(host)), port_(std::move(port)), factory_(std::move(factory)),
          connect_timeout_(connect_timeout)
    {
    }

    outbound_connection(const outbound_connection&) = delete;
    outbound_connection& operator=(const outbound_connection&) = delete;

    ~outbound_connection() { teardown(); }

    std::function<void()> on_open;
    std::function<void(std::string& in)> on_data;         // plaintext appended; erase what you consume
    std::function<void(outbound_status)> on_close;        // once, after which nothing else fires

    void start()
    {
        if (started_)
            return;
        started_ = true;
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (::getaddrinfo(host_.c_str(), port_.c_str(), &hints, &res) != 0 || !res)
            return finish(outbound_status::resolve_failed);
        for (addrinfo* a = res; a; a = a->ai_next) {
            if (a->ai_addrlen > sizeof(sockaddr_storage))
                continue;
            candidate c;
            c.family = a->ai_family;
            c.socktype = a->ai_socktype;
            c.protocol = a->ai_protocol;
            c.len = a->ai_addrlen;
            std::memcpy(&c.addr, a->ai_addr, a->ai_addrlen);
            addrs_.push_back(c);
        }
        ::freeaddrinfo(res);
        if (addrs_.empty())
            return finish(outbound_status::resolve_failed);
        if (connect_timeout_.count() > 0) {
            deadline_ = loop_->after(connect_timeout_);
            std::weak_ptr<outbound_connection> weak = shared_from_this();
            deadline_.on_fire([weak] {
                if (auto self = weak.lock())
                    if (!self->open_)
                        self->finish(outbound_status::timed_out);
            });
        }
        try_next_address();
    }

    // Queue `n` bytes of plaintext; false once the connection has failed
    bool write(const char* data, std::size_t n)
    {
        if (closed_ || fd_ < 0)
            return false;
        const bool ok = transport_ ? transport_->app_out(data, n, out_) : (out_.append(data, n), true);
        if (!ok) {
            finish(outbound_status::transport_error);
            return false;
        }
        if (open_)
            flush();
        return true;
    }
    bool write(const std::string& s) { return write(s.data(), s.size()); }

    // Close now; on_close fires with closed_by_us
    void close() { finish(outbound_status::closed_by_us); }

    // Send FIN after what is queued, keeping the read side open: a close frame then EOF, in that order
    void shutdown_write()
    {
        if (fd_ < 0 || closed_)
            return;
        if (backlog() == 0)
            ::shutdown(fd_, SHUT_WR);
        else
            shutdown_after_flush_ = true;
    }

    bool open() const noexcept { return open_; }
    bool closed() const noexcept { return closed_; }
    std::size_t backlog() const noexcept { return out_.size() - sent_; }
    const std::string& host() const noexcept { return host_; }
    const char* alpn() const noexcept { return transport_ ? transport_->alpn() : ""; }

private:
    struct candidate {
        int family = 0, socktype = 0, protocol = 0;
        sockaddr_storage addr{};
        socklen_t len = 0;
    };

    void try_next_address()
    {
        watch_.reset();
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        while (addr_at_ < addrs_.size()) {
            const candidate& c = addrs_[addr_at_++];
            const int s = ::socket(c.family, c.socktype, c.protocol);
            if (s < 0)
                continue;
            ::fcntl(s, F_SETFD, FD_CLOEXEC);
            ::fcntl(s, F_SETFL, O_NONBLOCK);
            const int on = 1;
            ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
#if defined(SO_NOSIGPIPE)
            ::setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
            const int rc = ::connect(s, reinterpret_cast<const sockaddr*>(&c.addr), c.len);
            if (rc != 0 && errno != EINPROGRESS) {
                ::close(s);
                continue;                       // refused outright: the next address
            }
            fd_ = s;
            in_.clear();
            out_.clear();
            sent_ = 0;
            // A fresh transport per attempt: TLS state cannot outlive its connection
            transport_.reset();
            if (factory_) {
                transport_ = factory_(host_);
                if (!transport_)
                    return finish(outbound_status::connect_failed);
            }
            watch_ = loop_->watch(fd_, fd_interest::write);
            std::weak_ptr<outbound_connection> weak = shared_from_this();
            watch_.on_writable([weak] {
                if (auto self = weak.lock())
                    self->on_writable();
            });
            watch_.on_readable([weak] {
                if (auto self = weak.lock())
                    self->on_readable();
            });
            watch_.on_error([weak] {
                if (auto self = weak.lock()) {
                    if (self->connecting_)
                        self->try_next_address();
                    else
                        self->on_readable();     // EOF can arrive as an error event: read what is buffered
                                                 // first, and recv() then says closed or the real error
                }
            });
            connecting_ = true;
            return;
        }
        finish(outbound_status::connect_failed);   // every address exhausted
    }

    void on_writable()
    {
        if (connecting_) {
            int err = 0;
            socklen_t len = sizeof err;
            if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0)
                return try_next_address();      // this address refused: the next
            connecting_ = false;
            if (transport_) {
                // TLS speaks first: the ClientHello goes before any application byte
                if (!transport_->start(out_))
                    return finish(outbound_status::handshake_failed);
                watch_.set_interest(fd_interest::read_write);
                flush();
                if (!closed_ && transport_->established())
                    become_open();
                return;
            }
            become_open();
            return;
        }
        flush();
    }

    void become_open()
    {
        open_ = true;
        deadline_.cancel();
        watch_.set_interest(backlog() > 0 ? fd_interest::read_write : fd_interest::read);
        auto keep = shared_from_this();     // a callback may drop the owner's pointer
        if (on_open)
            on_open();
        if (!closed_ && !out_.empty())
            flush();
    }

    void flush()
    {
        while (sent_ < out_.size()) {
            const ssize_t n = ::send(fd_, out_.data() + sent_, out_.size() - sent_, MSG_NOSIGNAL_FLAG);
            if (n > 0) {
                sent_ += std::size_t(n);
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                watch_.set_interest(fd_interest::read_write);
                return;
            }
            return finish(outbound_status::transport_error);
        }
        out_.clear();
        sent_ = 0;
        if (shutdown_after_flush_) {
            shutdown_after_flush_ = false;
            ::shutdown(fd_, SHUT_WR);
        }
        if (open_)
            watch_.set_interest(fd_interest::read);
    }

    void on_readable()
    {
        auto keep = shared_from_this();
        bool eof = false;
        for (;;) {
            char buf[16 * 1024];
            const ssize_t n = ::recv(fd_, buf, sizeof buf, 0);
            if (n > 0) {
                if (transport_) {
                    if (!transport_->wire_in(buf, std::size_t(n), in_, out_))
                        return finish(open_ ? outbound_status::transport_error : outbound_status::handshake_failed);
                } else {
                    in_.append(buf, std::size_t(n));
                }
                if (std::size_t(n) < sizeof buf)
                    break;
                continue;
            }
            if (n == 0) {
                eof = true;
                break;
            }
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            return finish(outbound_status::transport_error);
        }
        // The TLS handshake answers bytes with bytes; without this both ends wait
        if (transport_ && !out_.empty())
            flush();
        if (closed_)
            return;
        if (!open_ && transport_ && transport_->established())
            become_open();
        if (closed_)
            return;
        if (open_ && !in_.empty() && on_data)
            on_data(in_);
        if (closed_)
            return;
        if (eof)
            finish(open_ ? outbound_status::closed_by_peer : outbound_status::handshake_failed);
    }

    void finish(outbound_status why)
    {
        if (closed_)
            return;
        closed_ = true;
        open_ = false;
        teardown();
        auto cb = std::move(on_close);
        on_open = nullptr;
        on_data = nullptr;
        if (cb)
            cb(why);
    }

    void teardown() noexcept
    {
        deadline_.cancel();
        watch_.reset();
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

#if defined(MSG_NOSIGNAL)
    static constexpr int MSG_NOSIGNAL_FLAG = MSG_NOSIGNAL;
#else
    static constexpr int MSG_NOSIGNAL_FLAG = 0;
#endif

    event_loop* loop_;
    std::string host_, port_;
    transport_factory factory_;
    std::chrono::milliseconds connect_timeout_;
    std::vector<candidate> addrs_;
    std::size_t addr_at_ = 0;
    int fd_ = -1;
    std::unique_ptr<http::transport_delegate> transport_;
    event_loop::fd_watch watch_;
    event_loop::timer deadline_;
    std::string in_, out_;
    std::size_t sent_ = 0;
    bool started_ = false, connecting_ = false, open_ = false, closed_ = false;
    bool shutdown_after_flush_ = false;
};

} // namespace net
} // namespace snicholls

#endif // SNICHOLLS_HAS_EVENT_LOOP

#endif // ts_moveables_net_outbound_hpp
