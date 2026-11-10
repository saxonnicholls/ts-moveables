//
//  remote/discovery.hpp
//  TSMoveables - demos/remote_compute (from the remote_compute_example prototype at 92d8dc4)
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Workers announce themselves on the LAN; clients listen and add them.
//
//      worker:  beacon b(key, "pi-3", 7400, 4, "arm64");        // every 2 s
//      client:  c.discover(key);                                 // adds as they appear
//
//  A beacon is one UDP datagram to a multicast group (239.255.73.73:7399 by
//  default, TTL 1 so it never leaves the LAN), plain text:
//
//      snrem1 <name> <port> <cores> <arch> <protocol hex16> <mac hex32>
//
//  The mac is HMAC-SHA256 under the shared key, truncated to 16 bytes, so a
//  machine without the key cannot advertise itself into the cluster. A replayed
//  beacon only points the client at a real worker, which must still pass the
//  key handshake on connect, so replay gains an attacker nothing.
//
//  This is the idea of Bonjour/mDNS without the dependency: no daemon, no
//  library, identical on macOS and Linux, and testable on loopback.
//

#ifndef remote_discovery_hpp
#define remote_discovery_hpp

#include "sha256.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace snicholls::remote {

struct discovery_options {
    std::string group = "239.255.73.73";
    std::uint16_t port = 7399;
    std::chrono::milliseconds every{2000};
};

/// One machine as a beacon describes it.
struct advertised {
    std::string name;
    std::string address;   // where the datagram came from
    std::uint16_t port = 0;
    std::uint32_t cores = 0;
    std::string arch;
    std::uint64_t protocol = 0;
};

namespace detail {

inline std::string beacon_mac(std::string_view key, std::string_view body)
{
    const auto m = hmac_sha256(key, body);
    return to_hex(std::string_view(reinterpret_cast<const char*>(m.data()), 16));
}

inline std::string make_beacon(std::string_view key, const advertised& a)
{
    char sig[17];
    std::snprintf(sig, sizeof sig, "%016llx", static_cast<unsigned long long>(a.protocol));
    const std::string body = "snrem1 " + a.name + " " + std::to_string(a.port) + " " + std::to_string(a.cores) +
                             " " + a.arch + " " + sig;
    return body + " " + beacon_mac(key, body);
}

// nullopt-like: returns false for anything not a valid, correctly signed beacon
inline bool parse_beacon(std::string_view key, std::string_view text, advertised& out)
{
    const auto sp = text.rfind(' ');
    if (sp == std::string_view::npos)
        return false;
    const auto body = text.substr(0, sp);
    if (!equal_constant_time(text.substr(sp + 1), beacon_mac(key, body)))
        return false;
    std::istringstream in{std::string(body)};
    std::string magic, sig;
    unsigned port = 0;
    if (!(in >> magic >> out.name >> port >> out.cores >> out.arch >> sig) || magic != "snrem1" || port == 0 ||
        port > 65535 || sig.size() != 16)
        return false;
    out.port = static_cast<std::uint16_t>(port);
    out.protocol = std::stoull(sig, nullptr, 16);
    return true;
}

} // namespace detail

/// Announce a worker on the LAN until destroyed.
class beacon {
public:
    beacon(std::string key, advertised self, discovery_options opt = {})
        : key_(std::move(key)), self_(std::move(self)), opt_(std::move(opt))
    {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0)
            throw std::runtime_error("beacon: socket");
        const unsigned char ttl = 1, loop = 1;   // stay on the LAN; reach listeners on this host too
        ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
        ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
        to_.sin_family = AF_INET;
        to_.sin_port = htons(opt_.port);
        if (::inet_pton(AF_INET, opt_.group.c_str(), &to_.sin_addr) != 1)
            throw std::runtime_error("beacon: bad multicast group " + opt_.group);
        thread_ = std::thread([this] { run(); });
    }
    beacon(const beacon&) = delete;
    beacon& operator=(const beacon&) = delete;
    ~beacon()
    {
        stop_ = true;
        if (thread_.joinable())
            thread_.join();
        ::close(fd_);
    }

private:
    void run()
    {
        const std::string text = detail::make_beacon(key_, self_);
        constexpr std::chrono::milliseconds step{50};
        while (!stop_) {
            ::sendto(fd_, text.data(), text.size(), 0, reinterpret_cast<const sockaddr*>(&to_), sizeof to_);
            for (std::chrono::milliseconds waited{0}; waited < opt_.every && !stop_; waited += step)
                std::this_thread::sleep_for(step);
        }
    }

    std::string key_;
    advertised self_;
    discovery_options opt_;
    int fd_ = -1;
    sockaddr_in to_{};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

/// Listen for beacons and report each correctly signed one. Several listeners
/// on one host can share the port. Reports repeat every time a beacon arrives;
/// deduplicating is the receiver's business.
class beacon_listener {
public:
    using found_fn = std::function<void(const advertised&)>;

    beacon_listener(std::string key, found_fn found, discovery_options opt = {})
        : key_(std::move(key)), found_(std::move(found)), opt_(std::move(opt))
    {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0)
            throw std::runtime_error("beacon_listener: socket");
        const int on = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
#ifdef SO_REUSEPORT
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &on, sizeof on);
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(opt_.port);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
            ::close(fd_);
            throw std::runtime_error("beacon_listener: cannot bind UDP port " + std::to_string(opt_.port));
        }
        ip_mreq mreq{};
        ::inet_pton(AF_INET, opt_.group.c_str(), &mreq.imr_multiaddr);
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq) != 0) {
            ::close(fd_);
            throw std::runtime_error("beacon_listener: cannot join multicast group " + opt_.group);
        }
        thread_ = std::thread([this] { run(); });
    }
    beacon_listener(const beacon_listener&) = delete;
    beacon_listener& operator=(const beacon_listener&) = delete;
    ~beacon_listener()
    {
        stop_ = true;
        if (thread_.joinable())
            thread_.join();
        ::close(fd_);
    }

private:
    void run()
    {
        char buf[1024];
        while (!stop_) {
            pollfd p{fd_, POLLIN, 0};
            if (::poll(&p, 1, 100) <= 0)
                continue;
            sockaddr_in from{};
            socklen_t len = sizeof from;
            const auto n = ::recvfrom(fd_, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &len);
            if (n <= 0)
                continue;
            advertised a;
            if (!detail::parse_beacon(key_, std::string_view(buf, std::size_t(n)), a))
                continue;   // not ours, or not signed with our key
            char ip[INET_ADDRSTRLEN] = {};
            ::inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
            a.address = ip;
            found_(a);
        }
    }

    std::string key_;
    found_fn found_;
    discovery_options opt_;
    int fd_ = -1;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

} // namespace snicholls::remote

#endif /* remote_discovery_hpp */
