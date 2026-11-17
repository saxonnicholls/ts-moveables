//
//  http_health.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - /healthz and /readyz on this library's HTTP server,
//  for a load balancer or an orchestrator to poll:
//
//      auto health = std::make_shared<snicholls::health_registry>();
//      snicholls::serve_health(srv, health);
//
//  /healthz answers 200 while the process is live (not unhealthy) and 503
//  when it wants restarting; /readyz answers 200 while it should get traffic
//  (ready or degraded) and 503 otherwise, starting and stopping included.
//  Both send the report as JSON with Cache-Control: no-store.
//
//  The routes hold the registry by shared_ptr: the copy it replaces held
//  it by reference, so a registry destroyed before the server left the routes
//  pointing at freed memory (a hazard it documented rather than removed).
//
//  Ported from an earlier application framework's http_health.hpp. C++20 and POSIX, opt-in like fs/.
//

#ifndef ts_moveables_process_http_health_hpp
#define ts_moveables_process_http_health_hpp

#include <memory>
#include <string>
#include <utility>

#include "../http/server.hpp"
#include "health.hpp"

#if SNICHOLLS_HAS_HTTP_SERVER

namespace snicholls {

struct health_paths
{
    std::string live = "/healthz";
    std::string ready = "/readyz";
};

inline void serve_health(http::server& server, std::shared_ptr<const health_registry> health, health_paths paths = {})
{
    const auto answer = [](http::responder r, bool ok, std::string body) {
        http::response res(ok ? 200 : 503);
        res.set("Cache-Control", "no-store");
        res.content(std::move(body), "application/json");
        r.send(std::move(res));
    };
    server.get(std::move(paths.live), [health, answer](const http::request&, http::responder r) {
        const health_report report = health->report();
        answer(std::move(r), report.live(), report.to_json());
    });
    server.get(std::move(paths.ready), [health, answer](const http::request&, http::responder r) {
        const health_report report = health->report();
        answer(std::move(r), report.ready(), report.to_json());
    });
}

} // namespace snicholls

#endif // SNICHOLLS_HAS_HTTP_SERVER

#endif // ts_moveables_process_http_health_hpp
