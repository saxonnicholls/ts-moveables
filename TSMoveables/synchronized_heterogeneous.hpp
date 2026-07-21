//
//  synchronized_heterogeneous.hpp
//  TSMoveables
//
//  Created by Saxon Nicholls on 21/7/2026.
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables
//

#ifndef synchronized_heterogeneous_hpp
#define synchronized_heterogeneous_hpp

#include <algorithm>
#include <any>
#include <cstddef>
#include <functional>
#include <optional>
#include <tuple>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "synchronized.hpp"

namespace snicholls
{
    // Thread-safe heterogeneous containers, ready made.
    //
    // Each of these is synchronized<T> composed with a standard heterogeneous
    // type, plus the typed conveniences that make it pleasant to use. They all
    // inherit synchronized's guarantees: access only under the lock, closures
    // may not return references, and copy/move lock the source - so every one
    // of them is a moveable member of a moveable object.
    //
    // Reads that return values (try_get, holds, count, size) are snapshots:
    // true at the moment they were computed, possibly stale a moment later.
    // For read-modify-write, use the closure forms (visit, apply, with,
    // for_each) or fall back to with_lock for an arbitrary transaction.

    // Closed set of alternatives - exactly one of Ts... at a time
    template <typename... Ts>
    struct synchronized_variant : synchronized<std::variant<Ts...>> {

        using base_type = synchronized<std::variant<Ts...>>;
        using variant_type = std::variant<Ts...>;

        using base_type::base_type;
        using base_type::operator=;             // enables: sv = some_alternative;

        synchronized_variant() = default;

        // Visit the current alternative under the lock
        template <typename F>
        decltype(auto) visit(F&& f) {
            return this->with_lock([&](variant_type& var) -> decltype(auto) {
                return std::visit(std::forward<F>(f), var);
            });
        }

        template <typename F>
        decltype(auto) visit(F&& f) const {
            return this->with_lock([&](const variant_type& var) -> decltype(auto) {
                return std::visit(std::forward<F>(f), var);
            });
        }

        template <typename U>
        bool holds() const {
            return this->with_lock([](const variant_type& var) {
                return std::holds_alternative<U>(var);
            });
        }

        // Copy of the alternative if it is currently held
        template <typename U>
        std::optional<U> try_get() const {
            return this->with_lock([](const variant_type& var) -> std::optional<U> {
                if (auto p = std::get_if<U>(&var))
                    return *p;
                return std::nullopt;
            });
        }
    };

    // Fixed bundle - all of Ts..., accessed by index or by (unique) type
    template <typename... Ts>
    struct synchronized_tuple : synchronized<std::tuple<Ts...>> {

        using base_type = synchronized<std::tuple<Ts...>>;
        using tuple_type = std::tuple<Ts...>;

        using base_type::base_type;
        using base_type::operator=;

        synchronized_tuple() = default;

        template <std::size_t I>
        auto get() const {
            return this->with_lock([](const tuple_type& t) { return std::get<I>(t); });
        }

        template <typename U>
        U get() const {
            return this->with_lock([](const tuple_type& t) { return std::get<U>(t); });
        }

        template <std::size_t I, typename V>
        void set(V&& v) {
            this->with_lock([&](tuple_type& t) { std::get<I>(t) = std::forward<V>(v); });
        }

        template <typename U, typename V>
        void set(V&& v) {
            this->with_lock([&](tuple_type& t) { std::get<U>(t) = std::forward<V>(v); });
        }

        // Call f with every element, under one lock
        template <typename F>
        decltype(auto) apply(F&& f) {
            return this->with_lock([&](tuple_type& t) -> decltype(auto) {
                return std::apply(std::forward<F>(f), t);
            });
        }

        template <typename F>
        decltype(auto) apply(F&& f) const {
            return this->with_lock([&](const tuple_type& t) -> decltype(auto) {
                return std::apply(std::forward<F>(f), t);
            });
        }
    };

    // One value of unknown type
    struct synchronized_any : synchronized<std::any> {

        using base_type = synchronized<std::any>;

        using base_type::base_type;
        using base_type::operator=;

        synchronized_any() = default;

        template <typename U>
        bool holds() const {
            return this->with_lock([](const std::any& a) { return a.type() == typeid(U); });
        }

        template <typename U>
        std::optional<U> try_get() const {
            return this->with_lock([](const std::any& a) -> std::optional<U> {
                if (auto p = std::any_cast<U>(&a))
                    return *p;
                return std::nullopt;
            });
        }

        bool has_value() const {
            return this->with_lock([](const std::any& a) { return a.has_value(); });
        }

        void reset() {
            this->with_lock([](std::any& a) { a.reset(); });
        }
    };

    // At most one value per type - the blackboard / service-locator shape
    struct synchronized_type_map : synchronized<std::unordered_map<std::type_index, std::any>> {

        using base_type = synchronized<std::unordered_map<std::type_index, std::any>>;
        using map_type = std::unordered_map<std::type_index, std::any>;

        synchronized_type_map() = default;

        template <typename U>
        void put(U v) {
            this->with_lock([&](map_type& m) {
                m.insert_or_assign(std::type_index(typeid(U)), std::any(std::move(v)));
            });
        }

        template <typename U, typename... Args>
        void emplace(Args&&... args) {
            this->with_lock([&](map_type& m) {
                m[std::type_index(typeid(U))].template emplace<U>(std::forward<Args>(args)...);
            });
        }

        template <typename U>
        bool contains() const {
            return this->with_lock([](const map_type& m) {
                return m.count(std::type_index(typeid(U))) != 0;
            });
        }

        // Copy of the value if present
        template <typename U>
        std::optional<U> try_get() const {
            return this->with_lock([](const map_type& m) -> std::optional<U> {
                auto it = m.find(std::type_index(typeid(U)));
                if (it == m.end())
                    return std::nullopt;
                return *std::any_cast<U>(&it->second);
            });
        }

        // Run f on the stored U, in place under the lock, if present.
        // Returns whether f ran - the race-free read-modify-write path.
        template <typename U, typename F>
        bool with(F&& f) {
            return this->with_lock([&](map_type& m) {
                auto it = m.find(std::type_index(typeid(U)));
                if (it == m.end())
                    return false;
                std::invoke(std::forward<F>(f), *std::any_cast<U>(&it->second));
                return true;
            });
        }

        template <typename U, typename F>
        bool with(F&& f) const {
            return this->with_lock([&](const map_type& m) {
                auto it = m.find(std::type_index(typeid(U)));
                if (it == m.end())
                    return false;
                std::invoke(std::forward<F>(f), *std::any_cast<const U>(&it->second));
                return true;
            });
        }

        template <typename U>
        bool erase() {
            return this->with_lock([](map_type& m) {
                return m.erase(std::type_index(typeid(U))) != 0;
            });
        }

        std::size_t size() const {
            return this->with_lock([](const map_type& m) { return m.size(); });
        }

        bool empty() const {
            return this->with_lock([](const map_type& m) { return m.empty(); });
        }

        void clear() {
            this->with_lock([](map_type& m) { m.clear(); });
        }
    };

    // An open, ordered bag of values of any types
    struct synchronized_bag : synchronized<std::vector<std::any>> {

        using base_type = synchronized<std::vector<std::any>>;
        using bag_type = std::vector<std::any>;

        synchronized_bag() = default;

        template <typename U>
        void push(U v) {
            this->with_lock([&](bag_type& b) { b.emplace_back(std::move(v)); });
        }

        template <typename U>
        std::size_t count() const {
            return this->with_lock([](const bag_type& b) {
                return static_cast<std::size_t>(std::count_if(b.begin(), b.end(),
                    [](const std::any& a) { return a.type() == typeid(U); }));
            });
        }

        // Run f on every stored U, under one lock; returns how many matched
        template <typename U, typename F>
        std::size_t for_each(F&& f) {
            return this->with_lock([&](bag_type& b) {
                std::size_t n = 0;
                for (auto& a : b)
                    if (auto p = std::any_cast<U>(&a)) {
                        std::invoke(f, *p);
                        ++n;
                    }
                return n;
            });
        }

        template <typename U, typename F>
        std::size_t for_each(F&& f) const {
            return this->with_lock([&](const bag_type& b) {
                std::size_t n = 0;
                for (const auto& a : b)
                    if (auto p = std::any_cast<const U>(&a)) {
                        std::invoke(f, *p);
                        ++n;
                    }
                return n;
            });
        }

        // Remove every stored U and return them, in order
        template <typename U>
        std::vector<U> extract() {
            return this->with_lock([](bag_type& b) {
                std::vector<U> out;
                auto removed = std::remove_if(b.begin(), b.end(), [&](std::any& a) {
                    if (auto p = std::any_cast<U>(&a)) {
                        out.push_back(std::move(*p));
                        return true;
                    }
                    return false;
                });
                b.erase(removed, b.end());
                return out;
            });
        }

        std::size_t size() const {
            return this->with_lock([](const bag_type& b) { return b.size(); });
        }

        bool empty() const {
            return this->with_lock([](const bag_type& b) { return b.empty(); });
        }

        void clear() {
            this->with_lock([](bag_type& b) { b.clear(); });
        }
    };

} // namespace snicholls

#endif /* synchronized_heterogeneous_hpp */
