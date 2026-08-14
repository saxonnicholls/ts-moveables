//
//  ws_broadcast_hub.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - a webhook -> WebSocket broadcast hub
//
//  The pattern this file packages is everywhere: something outside the process
//  emits an event over HTTP (a webhook, a cron ping, another service), and a
//  crowd of browsers wants to see it live over WebSocket. The plumbing in the
//  middle is always the same - fan one message out to many sockets, keep a
//  little history so a page that just loaded is not blank, and never let one
//  slow reader stall the loop or grow memory without bound. So it is written
//  once, here, with no idea what the messages mean: topics are opaque strings
//  and payloads are opaque bytes. Point it at any source and it works.
//
//  It is built entirely on the library's own pieces. Fan-out reuses the
//  copy-on-write snapshot moveable_signal uses (grab the subscriber list under
//  a brief lock, then deliver without it). Per-topic history is a
//  snicholls::circular_buffer. Every counter is a moveable_atomic and every
//  lock a moveable_mutex, so the whole hub is one movable value over a shared
//  core - the same shape as http::server itself.
//
//      snicholls::http::server srv;
//      snicholls::http::ws_broadcast_hub hub;
//      hub.mount(srv);                          // GET /ws?topic=..  +  POST /ingest/:topic
//      srv.listen("0.0.0.0", 8080);
//      srv.run();
//
//      // from anywhere, at any time:
//      hub.publish("prices", "{\"BTC\":64000}");
//
//  Delivery envelope (frame_mode == envelope_json, the default). Each WebSocket
//  text frame a subscriber receives is exactly one JSON object:
//
//      {"seq":<uint64>,"ts_ms":<uint64>,"topic":"<string>","payload":"<string>"}
//
//    seq      global, monotonic, starts at 1; one per published message, in
//             delivery order (so a client can spot a gap or a reorder)
//    ts_ms    server wall-clock time of publication, Unix epoch milliseconds
//    topic    the topic the message was published to (JSON-string escaped)
//    payload  the raw published bytes, carried verbatim as a JSON-string value
//             (escaped so any UTF-8 text is legal JSON). Not re-parsed - to the
//             hub it is an opaque string.
//
//  In frame_mode == raw the payload bytes are sent verbatim as the frame, with
//  no envelope - for a source that already frames its own messages.
//
//  Guarded on SNICHOLLS_HAS_WEBSOCKET: where the WebSocket delegate compiles to
//  nothing (non-POSIX in phase 1) so does this.
//

#pragma once

#include "websocket.hpp"

#if !SNICHOLLS_HAS_WEBSOCKET
#define SNICHOLLS_HAS_WS_BROADCAST_HUB 0
#else
#define SNICHOLLS_HAS_WS_BROADCAST_HUB 1

#include "../concurrent/circular_buffer.hpp"
#include "../moveable/atomic.hpp"
#include "../moveable/mutex.hpp"
#include "../utils/json.hpp"
#include "../utils/time.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace snicholls {
namespace http {

namespace detail {

// Both of these used to be written out here. They now live in utils/ because
// the logger had its own copy of the escaper and the two had already drifted -
// see utils/json.hpp for which way, and why that kind of duplicate survives.
using ::snicholls::utils::json_escape;
using ::snicholls::utils::unix_millis;

} // namespace detail

// How each delivered frame is shaped (see the file header for the envelope).
enum class ws_hub_framing : std::uint8_t {
    envelope_json,      // {"seq":..,"ts_ms":..,"topic":"..","payload":".."}
    raw                 // payload bytes sent verbatim, no envelope
};

// The hub's knobs. Defined at namespace scope (aliased as ws_broadcast_hub::
// config below) so its default member initializers are complete before the hub
// class is defined - a nested type's initializers cannot be value-initialised
// while the enclosing class is still being parsed.
struct ws_hub_config {
    // Topic that receives every published message regardless of its topic.
    // A client connecting with this topic is a firehose subscriber.
    std::string    wildcard_topic    = "*";

    std::size_t    max_subscribers   = 10000;             // total, across all topics
    std::size_t    ring_capacity     = 64;                // per-topic replay history (rounded up to a power of two)
    std::size_t    max_message_bytes = 1u * 1024 * 1024;  // ingest + frame size cap

    // Per-connection outbound bound. A reader slower than the publish rate has
    // its queue trimmed oldest-first, never grown without limit.
    std::size_t    max_queue_msgs    = 256;
    std::size_t    max_queue_bytes   = 8u * 1024 * 1024;
    // Stop handing frames to a socket once this many bytes are already queued
    // for it; resume as it drains. Backpressure, not blocking.
    std::size_t    send_high_water   = 1u * 1024 * 1024;

    bool           replay_on_connect = true;
    ws_hub_framing frame_mode        = ws_hub_framing::envelope_json;
};

struct ws_hub_stats {
    std::uint64_t published   = 0;      // messages accepted by publish()
    std::uint64_t delivered   = 0;      // frames handed to a socket
    std::uint64_t dropped     = 0;      // frames trimmed by backpressure
    std::uint64_t subscribers = 0;      // live subscribers now
    std::uint64_t seq         = 0;      // last sequence number issued
};

// ---------------------------------------------------------------------- the hub
//
// Movable, like http::server: a cheap handle over a shared core, so it can be
// built in a factory and moved into place, and the route handlers it hands out
// keep the core alive for exactly as long as the server holds them. Copying is
// deleted - two handles publishing into one core is fine, but that is what a
// deliberate share() would be, not an accidental copy.

class ws_broadcast_hub {
public:
    using framing = ws_hub_framing;     // ws_broadcast_hub::framing::{envelope_json,raw}
    using config  = ws_hub_config;      // ws_broadcast_hub::config
    using stats   = ws_hub_stats;       // ws_broadcast_hub::stats

    ws_broadcast_hub() : ws_broadcast_hub(config{}) {}
    explicit ws_broadcast_hub(config cfg)
        : h_(std::make_shared<impl>(std::move(cfg))) {}

    ws_broadcast_hub(ws_broadcast_hub&&) noexcept = default;
    ws_broadcast_hub& operator=(ws_broadcast_hub&&) noexcept = default;
    ws_broadcast_hub(const ws_broadcast_hub&) = delete;
    ws_broadcast_hub& operator=(const ws_broadcast_hub&) = delete;

    // Fan a message out to every subscriber of `topic` and every wildcard
    // subscriber, and record it in that topic's replay history. O(subscribers),
    // never blocks: a slow reader is trimmed, not waited on. Safe from any
    // thread - if the hub was mount()ed and this is a foreign thread, the
    // delivery is marshalled onto the loop so backpressure reads stay accurate.
    // Returns the number of connected subscribers the message was fanned out to.
    std::size_t publish(std::string_view topic, std::string_view payload)
    {
        return h_->publish(topic, payload);
    }

    // A websocket_route for GET: the topic is the `topic` query parameter
    // (default: the wildcard topic). On connect the client is registered, the
    // topic's replay history is streamed, then live publishes follow in order.
    handler ws_route() const
    {
        auto h = h_;
        ws_config wscfg;
        wscfg.max_message = h_->cfg.max_message_bytes;
        wscfg.max_frame   = h_->cfg.max_message_bytes;
        return websocket_route([h](websocket ws) { h->on_connect(std::move(ws)); }, wscfg);
    }

    // A plain HTTP handler for the ingest side: the request body is the payload,
    // the topic is the `:topic` path parameter if the route has one, else the
    // `topic` query parameter, else the wildcard topic. Answers 202 immediately
    // (the raw "webhook -> ws pump" case). Reusable for any POST source.
    handler ingest_handler() const
    {
        auto h = h_;
        return [h](const request& req, responder res) {
            std::string topic = req.has_param("topic") ? req.param("topic")
                                                       : req.query_param("topic");
            if (topic.empty())
                topic = h->cfg.wildcard_topic;
            accept(h, topic, req, std::move(res));
        };
    }

    // Fixed-topic ingest: every POST to this handler publishes to `topic`,
    // regardless of path or query. Register it on as many distinct paths as you
    // like to run several independent webhook endpoints on one server -
    //     srv.post("/hook1", hub.ingest_handler("hook1"));
    //     srv.post("/hook2", hub.ingest_handler("hook2"));
    // each feeding its own topic (and the wildcard firehose). Call mount() once
    // first (or set the poster) if you also publish() from a foreign thread.
    handler ingest_handler(std::string topic) const
    {
        auto h = h_;
        auto t = std::make_shared<const std::string>(std::move(topic));
        return [h, t](const request& req, responder res) {
            accept(h, *t, req, std::move(res));
        };
    }

    // Register both sides on a server in one call, and remember the loop so
    // publish() from a worker thread can marshal onto it. Call before run()
    // (or from the loop thread after).
    void mount(server& srv,
               const std::string& ws_path     = "/ws",
               const std::string& ingest_path = "/ingest/:topic")
    {
        h_->poster = srv.loop().make_poster();
        srv.get(ws_path, ws_route());
        srv.post(ingest_path, ingest_handler());
    }

    std::size_t subscribers() const { return static_cast<std::size_t>(h_->sub_count.load()); }
    std::size_t subscribers(std::string_view topic) const { return h_->topic_size(topic); }
    stats snapshot() const { return h_->snapshot(); }
    const config& configuration() const { return h_->cfg; }

private:
    // One connected client. Never copied or moved - always held by shared_ptr,
    // so the on_close slot can weak-reference it and the fan-out can hold it
    // alive across the brief window between snapshot and delivery. Its queue is
    // guarded by its own mutex, so publishes to different clients never contend.
    // One immutable framed message, shared across every subscriber's queue and
    // the replay ring — a publish copies a shared_ptr, never the frame bytes.
    using frame_ptr = std::shared_ptr<const std::string>;

    struct subscriber {
        websocket                ws;
        std::string              topic;
        moveable_mutex<>         mtx;
        std::deque<frame_ptr>    queue;                 // shared frames awaiting the socket
        std::size_t              queued_bytes = 0;
        moveable_atomic_uint64_t sent{0};
        moveable_atomic_uint64_t dropped{0};
    };

    using sub_ptr      = std::shared_ptr<subscriber>;
    using sub_list     = std::vector<sub_ptr>;
    using sub_snapshot = std::shared_ptr<const sub_list>;    // copy-on-write, like moveable_signal

    struct impl : std::enable_shared_from_this<impl> {
        config cfg;

        moveable_mutex<> mtx;                                        // guards topics + rings
        std::unordered_map<std::string, sub_snapshot>              topics;   // topic -> subscriber snapshot
        std::unordered_map<std::string, circular_buffer<frame_ptr>> rings; // topic -> replay history

        event_loop::poster       poster;                            // set by mount(); may be empty

        moveable_atomic_uint64_t seq{0};
        moveable_atomic_uint64_t sub_count{0};
        moveable_atomic_uint64_t published{0};
        moveable_atomic_uint64_t delivered{0};
        moveable_atomic_uint64_t dropped{0};

        explicit impl(config c) : cfg(std::move(c))
        {
            if (cfg.ring_capacity == 0)
                cfg.ring_capacity = 1;              // circular_buffer needs a positive capacity
        }

        // -------------------------------------------------------- publish path

        std::size_t publish(std::string_view topic_v, std::string_view payload_v)
        {
            // From a foreign thread on a mounted hub, hop onto the loop so that
            // send_high_water sees each socket's true backlog (which the loop
            // publishes) rather than a stale value. The fan-out count is still
            // computed synchronously so the caller gets a real answer.
            if (poster.valid() && !poster.on_loop_thread()) {
                std::string topic(topic_v), payload(payload_v);
                const std::size_t n = count_targets(topic);
                auto self = shared_from_this();
                self->poster.post([self, topic = std::move(topic),
                                   payload = std::move(payload)]() mutable {
                    self->do_publish(topic, payload);
                });
                return n;
            }
            return do_publish(std::string(topic_v), std::string(payload_v));
        }

        std::size_t do_publish(const std::string& topic, const std::string& payload)
        {
            sub_snapshot topic_subs, wild_subs;
            frame_ptr msg;
            {
                std::lock_guard<moveable_mutex<>> g(mtx);
                const std::uint64_t s  = seq.fetch_add(1) + 1;      // 1-based, ordered under the lock
                const std::uint64_t ts = detail::unix_millis();
                msg = std::make_shared<const std::string>(frame(topic, payload, s, ts));

                if (cfg.replay_on_connect) {
                    ring_push(ring_for(topic), msg);
                    if (topic != cfg.wildcard_topic)
                        ring_push(ring_for(cfg.wildcard_topic), msg);
                }
                topic_subs = snapshot_for(topic);
                if (topic != cfg.wildcard_topic)
                    wild_subs = snapshot_for(cfg.wildcard_topic);
            }
            published.fetch_add(1);

            std::size_t n = 0;
            std::vector<subscriber*> dead;
            auto fan = [&](const sub_list& list) {
                for (const auto& sp : list) {
                    if (!sp->ws.connected()) {          // abrupt disconnects have no close frame
                        dead.push_back(sp.get());
                        continue;
                    }
                    deliver(*sp, msg);
                    ++n;
                }
            };
            if (topic_subs) fan(*topic_subs);
            if (wild_subs)  fan(*wild_subs);
            if (!dead.empty())
                for (auto* p : dead) remove(p);          // lazy reap of dead sockets
            return n;
        }

        std::string frame(const std::string& topic, const std::string& payload,
                          std::uint64_t s, std::uint64_t ts)
        {
            if (cfg.frame_mode == framing::raw)
                return payload;
            std::string e;
            e.reserve(payload.size() + topic.size() + 64);
            e += "{\"seq\":";      detail::append_uint(e, static_cast<unsigned long long>(s));
            e += ",\"ts_ms\":";    detail::append_uint(e, static_cast<unsigned long long>(ts));
            e += ",\"topic\":\"";  detail::json_escape(topic, e);   e += '"';
            e += ",\"payload\":\"";detail::json_escape(payload, e); e += "\"}";
            return e;
        }

        // Enqueue one frame for one subscriber and push what the socket will
        // take. Trim oldest-first if the queue is over its bound. Loop-agnostic:
        // the per-subscriber lock makes it safe from any calling thread.
        void deliver(subscriber& sub, const frame_ptr& msg)
        {
            std::lock_guard<moveable_mutex<>> g(sub.mtx);
            sub.queue.push_back(msg);                    // shares the frame, no byte copy
            sub.queued_bytes += msg->size();
            while (sub.queue.size() > cfg.max_queue_msgs ||
                   (sub.queued_bytes > cfg.max_queue_bytes && sub.queue.size() > 1)) {
                sub.queued_bytes -= sub.queue.front()->size();
                sub.queue.pop_front();
                sub.dropped.fetch_add(1);
                dropped.fetch_add(1);
            }
            pump_locked(sub);
        }

        // Hand queued frames to the socket while it is keeping up. send_text is
        // non-blocking (it queues or marshals), so this never stalls the loop.
        void pump_locked(subscriber& sub)
        {
            while (!sub.queue.empty() && sub.ws.backlog() < cfg.send_high_water) {
                frame_ptr m = std::move(sub.queue.front());
                sub.queue.pop_front();
                sub.queued_bytes -= m->size();
                const bool ok = sub.ws.send_text_shared(std::move(m));   // no per-socket payload copy
                sub.sent.fetch_add(1);
                delivered.fetch_add(1);
                if (!ok)
                    break;                              // socket gone; the rest is reaped later
            }
        }

        // ------------------------------------------------------ connect / close

        void on_connect(websocket ws)
        {
            std::string topic = ws.handshake().query_param("topic");
            if (topic.empty())
                topic = cfg.wildcard_topic;

            auto sub = std::make_shared<subscriber>();
            sub->ws    = ws.share();
            sub->topic = topic;

            bool over = false;
            {
                std::lock_guard<moveable_mutex<>> g(mtx);
                if (sub_count.load() >= cfg.max_subscribers) {
                    over = true;
                } else {
                    // Seed the replay history while the subscriber is still
                    // invisible to publishers, then register it - both under the
                    // one lock. So any publish is ordered strictly before or
                    // after this pair: it either lands in the history we just
                    // copied, or is delivered live after it. No gap, no dup, and
                    // replay always precedes live in the queue.
                    if (cfg.replay_on_connect) {
                        std::vector<frame_ptr> hist;
                        ring_snapshot(ring_for(topic), hist);
                        for (auto& m : hist) {
                            sub->queued_bytes += m->size();
                            sub->queue.push_back(std::move(m));
                        }
                        while (sub->queue.size() > cfg.max_queue_msgs) {
                            sub->queued_bytes -= sub->queue.front()->size();
                            sub->queue.pop_front();
                        }
                    }
                    register_locked(topic, sub);
                    sub_count.fetch_add(1);
                }
            }
            if (over) {
                ws.close(ws_policy_violation, "subscriber limit reached");
                return;
            }

            // Unsubscribe on a graceful close. Weak on both sides: a strong
            // subscriber here would be owned by the ws_state that owns this
            // slot, and neither would ever free (the library's own cycle rule).
            std::weak_ptr<subscriber> wsub  = sub;
            std::weak_ptr<impl>       wself = weak_from_this();
            ws.on_close([wself, wsub](websocket, std::uint16_t, const std::string&) {
                auto self = wself.lock();
                auto s    = wsub.lock();
                if (self && s)
                    self->remove(s.get());
            });

            std::lock_guard<moveable_mutex<>> g(sub->mtx);   // drain the replay now
            pump_locked(*sub);
        }

        // caller holds mtx
        void register_locked(const std::string& topic, const sub_ptr& sub)
        {
            auto cur  = snapshot_for(topic);
            auto next = std::make_shared<sub_list>();
            if (cur) {
                next->reserve(cur->size() + 1);
                *next = *cur;
            }
            next->push_back(sub);
            topics[topic] = std::move(next);
        }

        void remove(subscriber* ptr)
        {
            std::lock_guard<moveable_mutex<>> g(mtx);
            auto it = topics.find(ptr->topic);
            if (it == topics.end() || !it->second)
                return;
            const sub_list& cur = *it->second;
            auto next = std::make_shared<sub_list>();
            next->reserve(cur.size());
            bool removed = false;
            for (const auto& sp : cur) {
                if (sp.get() == ptr)
                    removed = true;
                else
                    next->push_back(sp);
            }
            if (next->empty())
                topics.erase(it);
            else
                it->second = std::move(next);
            if (removed)
                sub_count.fetch_sub(1);
        }

        // ------------------------------------------------------------- queries

        std::size_t count_targets(const std::string& topic)
        {
            std::lock_guard<moveable_mutex<>> g(mtx);
            std::size_t n = 0;
            if (auto s = snapshot_for(topic))
                n += s->size();
            if (topic != cfg.wildcard_topic)
                if (auto w = snapshot_for(cfg.wildcard_topic))
                    n += w->size();
            return n;
        }

        std::size_t topic_size(std::string_view topic)
        {
            std::lock_guard<moveable_mutex<>> g(mtx);
            auto it = topics.find(std::string(topic));
            return (it == topics.end() || !it->second) ? 0 : it->second->size();
        }

        stats snapshot() const
        {
            stats s;
            s.published   = published.load();
            s.delivered   = delivered.load();
            s.dropped     = dropped.load();
            s.subscribers = sub_count.load();
            s.seq         = seq.load();
            return s;
        }

        // --------------------------------------------------- internals (mtx held)

        sub_snapshot snapshot_for(const std::string& topic)
        {
            auto it = topics.find(topic);
            return it == topics.end() ? nullptr : it->second;
        }

        circular_buffer<frame_ptr>& ring_for(const std::string& topic)
        {
            auto it = rings.find(topic);
            if (it == rings.end())
                it = rings.emplace(std::piecewise_construct,
                                   std::forward_as_tuple(topic),
                                   std::forward_as_tuple(cfg.ring_capacity)).first;
            return it->second;
        }

        // Keep the last N: overwrite the oldest when full. Single-threaded here
        // (mtx held), so the SPSC ring's push/pop are used without contention.
        static void ring_push(circular_buffer<frame_ptr>& cb, const frame_ptr& msg)
        {
            if (cb.full()) {
                frame_ptr discard;
                cb.try_pop(discard);
            }
            cb.try_push(msg);
        }

        // Non-destructive read of the whole ring, oldest -> newest: drain it and
        // refill it in the same order. Safe because mtx serialises all access.
        static void ring_snapshot(circular_buffer<frame_ptr>& cb, std::vector<frame_ptr>& out)
        {
            frame_ptr s;
            while (cb.try_pop(s))
                out.push_back(std::move(s));
            for (const auto& m : out)
                cb.try_push(m);
        }
    };

    // Publish the body to `topic` and answer 202 with the fan-out count. Shared
    // by both ingest_handler overloads so the accept semantics stay in one place.
    static void accept(const std::shared_ptr<impl>& h, const std::string& topic,
                       const request& req, responder res)
    {
        if (req.body.size() > h->cfg.max_message_bytes) {
            res.send(413, "text/plain; charset=utf-8", "413 Payload Too Large\n");
            return;
        }
        const std::size_t n = h->publish(topic, req.body);

        response r(202);
        std::string b = "{\"accepted\":true,\"topic\":\"";
        detail::json_escape(topic, b);
        b += "\",\"subscribers\":";
        detail::append_uint(b, static_cast<unsigned long long>(n));
        b += "}\n";
        r.content(std::move(b), "application/json");
        res.send(std::move(r));
    }

    std::shared_ptr<impl> h_;
};

} // namespace http
} // namespace snicholls

#endif // SNICHOLLS_HAS_WEBSOCKET
