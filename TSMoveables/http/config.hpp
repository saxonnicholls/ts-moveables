//
//  http/config.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  The knobs, and what the observation taps carry.
//

#ifndef http_config_hpp
#define http_config_hpp

#include "parser.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace snicholls {
namespace http {

// ------------------------------------------------------------------ the knobs

struct server_config {
    parse_limits limits{};
    std::size_t read_buffer      = 64u * 1024;      // one shared buffer, not one per connection
    std::size_t write_high_water = 1024u * 1024;    // pause reading above this backlog
    int backlog                  = 1024;
    int accept_burst             = 64;              // accepts per readiness, so one loop cannot starve others
    int reads_per_event          = 8;
    std::chrono::seconds idle_timeout{60};
    std::chrono::seconds request_timeout{20};       // slowloris
    std::chrono::milliseconds sweep_interval{1000};
    bool tcp_nodelay             = true;
    bool reuse_port              = false;           // one server per thread, kernel-balanced
    std::size_t max_connections  = 0;               // 0: unlimited
    std::string server_name      = "ts-moveables";
};

// ---------------------------------------------------------------- observation

struct connection_info {
    int fd = -1;
    std::string peer;
    std::uint16_t peer_port = 0;
    std::uint64_t id = 0;
};

struct access_entry {
    std::string method;
    std::string path;
    std::string query;
    int status = 0;
    std::size_t response_bytes = 0;
    std::size_t request_bytes = 0;
    double duration_ms = 0.0;
    int fd = -1;
    std::uint64_t stream = 0;
};

} // namespace http
} // namespace snicholls

#endif /* http_config_hpp */
