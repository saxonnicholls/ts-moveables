//
//  intern_pool.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - content-addressed interning ("hash-consing").
//
//  Identical values collapse to a single shared, immutable instance, so equal
//  data is stored once and can be compared by pointer. Plug-and-play for any
//  hashable, equality-comparable T - frames, packets, structs. It turns "N
//  copies of the same bytes" into "one buffer, N pointers", which is the natural
//  companion to the ws_broadcast_hub's shared-frame fan-out: the hub already
//  hands every subscriber a shared_ptr to one frame; interning makes that
//  pointer canonical across time and across publishes too (a re-sent webhook,
//  a heartbeat, an unchanged status - one buffer, however many times it arrives).
//
//  Self-cleaning: the pool holds WEAK references, so a value is freed as soon as
//  the last user drops it. Memory is bounded by the live working set, never by
//  history - there is no "cache eviction" to tune and no unbounded growth.
//
//  Why exact equality, not just a hash (this matters wherever a wrong alias is
//  costly): two DIFFERENT payloads must never alias to one pointer. A 64-bit hash has
//  birthday collisions around 2^32 live values; a Bloom filter has false
//  positives by construction. So here the hash only selects a bucket and an
//  exact `Eq` compare decides membership - the hash is the accelerator, equality
//  is the authority. (A Bloom filter is a legitimate *negative* pre-check in
//  front of this - "definitely new, skip the lookup" - but can never BE the
//  check, because a false "seen" would silently drop or mis-alias a real value.)
//
//  Thread-safe under one moveable_mutex. The hot path is one lock + one hash probe; a hit
//  returns the existing pointer (and frees the duplicate), a miss allocates once.
//  Shard by hash if a single lock ever becomes the bottleneck.
//

#pragma once

#include "../moveable/mutex.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace snicholls
{

template <class T, class Hash = std::hash<T>, class Eq = std::equal_to<T>>
class intern_pool
{
public:
    using value_type = T;
    using ptr        = std::shared_ptr<const T>;

    struct stats {
        std::uint64_t interned = 0;   // total intern() calls
        std::uint64_t hits     = 0;   // calls that reused an existing instance
        std::size_t   live     = 0;   // distinct values currently alive
    };

    intern_pool() = default;
    explicit intern_pool(std::size_t bucket_hint) { table_.reserve(bucket_hint); }

    // Copy is deleted, move is not, and the difference is the whole point.
    //
    // Copying would fork the identity domain: two pools, each handing out its
    // own canonical pointer for the same bytes, so `a == b` would stop implying
    // `ptr_a == ptr_b` - the one guarantee this class exists to make.
    //
    // Moving forks nothing. It relocates the single pool; the table travels with
    // it, and every `shared_ptr<const T>` already handed out stays valid and
    // stays canonical, because those point at the interned values and never at
    // the pool. So a class holding an intern_pool keeps the rule of zero, which
    // is the reason this library exists - a component of ours that could not be
    // a member of a moveable object would be arguing against its own thesis.
    //
    // The move is checked rather than trusted: `moveable_mutex` throws if it is
    // held, so moving a pool out from under a thread inside intern() is loud
    // instead of undefined. Not noexcept, for exactly that reason.
    intern_pool(const intern_pool&)            = delete;
    intern_pool& operator=(const intern_pool&) = delete;
    intern_pool(intern_pool&&)                 = default;
    intern_pool& operator=(intern_pool&&)      = default;

    // Return the one canonical, immutable instance equal to `value`. If an equal
    // instance is already alive, returns it and discards `value`'s buffer;
    // otherwise adopts `value`. While any result is still referenced, every
    // equal input returns a pointer-identical result: (a == b) ⇔ (ptr_a == ptr_b).
    ptr intern(T value)
    {
        const std::size_t h = hash_(value);
        std::lock_guard<moveable_mutex<>> g(mtx_);
        ++interned_;

        auto range = table_.equal_range(h);
        for (auto it = range.first; it != range.second;) {
            if (ptr sp = it->second.lock()) {
                if (eq_(*sp, value)) {          // hash matched AND bytes equal
                    ++hits_;
                    return sp;                  // duplicate `value` freed on return
                }
                ++it;
            } else {
                it = table_.erase(it);          // prune a dead weak ref in passing
            }
        }

        ptr sp = std::make_shared<const T>(std::move(value));
        table_.emplace(h, std::weak_ptr<const T>(sp));
        maybe_sweep_();
        return sp;
    }

    // Intern without adopting: return the canonical instance if one is alive,
    // else nullptr. A read-only probe (no allocation, no insert).
    ptr find(const T& value) const
    {
        const std::size_t h = hash_(value);
        std::lock_guard<moveable_mutex<>> g(mtx_);
        auto range = table_.equal_range(h);
        for (auto it = range.first; it != range.second; ++it)
            if (ptr sp = it->second.lock())
                if (eq_(*sp, value))
                    return sp;
        return nullptr;
    }

    stats snapshot() const
    {
        std::lock_guard<moveable_mutex<>> g(mtx_);
        stats s;
        s.interned = interned_;
        s.hits     = hits_;
        for (const auto& kv : table_)
            if (!kv.second.expired())
                ++s.live;
        return s;
    }

    // Drop expired entries now. intern() does this lazily and amortised; exposed
    // for tests and for ops that want to reclaim table slots on demand.
    void sweep()
    {
        std::lock_guard<moveable_mutex<>> g(mtx_);
        sweep_locked_();
    }

private:
    void sweep_locked_()
    {
        for (auto it = table_.begin(); it != table_.end();) {
            if (it->second.expired())
                it = table_.erase(it);
            else
                ++it;
        }
        since_sweep_ = 0;
    }

    // Amortised cleanup: once we have inserted about as many entries as the table
    // holds, walk it once. Dead weak refs therefore cost O(1) amortised per
    // insert and can never accumulate without bound.
    void maybe_sweep_()
    {
        if (++since_sweep_ >= table_.size())
            sweep_locked_();
    }

    mutable moveable_mutex<>                                    mtx_;
    std::unordered_multimap<std::size_t, std::weak_ptr<const T>> table_;
    Hash          hash_{};
    Eq            eq_{};
    std::uint64_t interned_    = 0;
    std::uint64_t hits_        = 0;
    std::size_t   since_sweep_ = 0;
};

} // namespace snicholls
