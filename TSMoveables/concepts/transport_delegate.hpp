//
//  concepts/transport_delegate.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  The concept twin of interfaces/transport_delegate.hpp: "how bytes reach a
//  connection" (plaintext, or TLS and its handshake) as a compile-time
//  constraint instead of a vtable.
//
//  The interface stays the right tool where a transport is chosen at run time
//  and held by pointer, which is every connection: net::transport_factory
//  returns a unique_ptr<transport_delegate>, and the server, the HTTP client
//  and the WebSocket client hold one. The concept serves the other side: a
//  backend written as a plain class, with no vtable, that is still checked
//  against the contract at compile time, and handed to that code through an
//  adapter.
//
//    transport_like<T>            the members of transport_delegate as a
//                                 concept, with their exact types, const and
//                                 noexcept. The interface satisfies it
//                                 (asserted at the bottom), so a member changed
//                                 on one side and not the other fails here.
//    is_transport_like_v<T>       the same test as a C++17 trait.
//    transport_adapter<Impl>      concept to interface: a transport_delegate
//                                 that forwards to a model, held by value, or
//                                 bound to one when Impl is a reference.
//    delegate_transport           interface to concept: a value type holding a
//                                 unique_ptr<transport_delegate> (what a
//                                 transport_factory returns), for templates
//                                 written over transport_like.
//    make_transport_factory<Impl> a net::transport_factory-shaped callable that
//                                 builds transport_adapter<Impl>s from a
//                                 hostname (net/outbound.hpp names the type;
//                                 this header returns the lambda, so it needs
//                                 no event loop).
//
//  The concept is syntactic only. The semantics - wire_in appends plaintext
//  and owes-the-peer bytes, app_out appends wire bytes, start speaks first for
//  a client, alpn is empty when nothing was negotiated - are the interface's
//  contract, and interfaces/transport_delegate_conformance.hpp is the one kit that checks
//  them for any pair, either form.
//
//  C++17 like the interface; the concept itself needs C++20 concepts.
//

#ifndef concepts_transport_delegate_hpp
#define concepts_transport_delegate_hpp

#include <cstddef>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include "../interfaces/transport_delegate.hpp"
#include "detect.hpp"

namespace snicholls {
namespace http {

namespace detail {

template <typename T, typename = void>
struct is_transport_like : std::false_type {};

template <typename T>
struct is_transport_like<T, std::void_t<
    decltype(std::declval<const T&>().name()),
    decltype(std::declval<T&>().wire_in(std::declval<const char*>(), std::size_t{}, std::declval<std::string&>(),
                                        std::declval<std::string&>())),
    decltype(std::declval<T&>().app_out(std::declval<const char*>(), std::size_t{}, std::declval<std::string&>())),
    decltype(std::declval<T&>().start(std::declval<std::string&>())),
    decltype(std::declval<const T&>().established()),
    decltype(std::declval<const T&>().alpn()),
    decltype(std::declval<T&>().shutdown(std::declval<std::string&>()))>>
    : std::bool_constant<
          std::is_same_v<decltype(std::declval<const T&>().name()), const char*> &&
          noexcept(std::declval<const T&>().name()) &&
          std::is_same_v<decltype(std::declval<T&>().wire_in(std::declval<const char*>(), std::size_t{},
                                                             std::declval<std::string&>(),
                                                             std::declval<std::string&>())),
                         bool> &&
          std::is_same_v<decltype(std::declval<T&>().app_out(std::declval<const char*>(), std::size_t{},
                                                             std::declval<std::string&>())),
                         bool> &&
          std::is_same_v<decltype(std::declval<T&>().start(std::declval<std::string&>())), bool> &&
          std::is_same_v<decltype(std::declval<const T&>().established()), bool> &&
          noexcept(std::declval<const T&>().established()) &&
          std::is_same_v<decltype(std::declval<const T&>().alpn()), const char*> &&
          noexcept(std::declval<const T&>().alpn()) &&
          std::is_same_v<decltype(std::declval<T&>().shutdown(std::declval<std::string&>())), void>> {};

} // namespace detail

// Whether T has transport_delegate's members: the C++17 spelling of transport_like
template <typename T>
inline constexpr bool is_transport_like_v = detail::is_transport_like<std::remove_cv_t<T>>::value;

#if TS_MOVEABLES_HAS_CONCEPTS
template <typename T>
concept transport_like =
    requires(const T& t) {
        { t.name() } noexcept -> std::same_as<const char*>;
        { t.established() } noexcept -> std::same_as<bool>;
        { t.alpn() } noexcept -> std::same_as<const char*>;
    } &&
    requires(T& t, const char* data, std::size_t n, std::string& a, std::string& b) {
        { t.wire_in(data, n, a, b) } -> std::same_as<bool>;
        { t.app_out(data, n, b) } -> std::same_as<bool>;
        { t.start(b) } -> std::same_as<bool>;
        { t.shutdown(b) } -> std::same_as<void>;
    };
#define TS_MOVEABLES_TRANSPORT ::snicholls::http::transport_like
#else
#define TS_MOVEABLES_TRANSPORT typename
#endif

// Concept to interface: transport_delegate implemented by forwarding to a model
template <typename Impl>
class transport_adapter final : public transport_delegate {
    static_assert(is_transport_like_v<std::remove_reference_t<Impl>>,
                  "snicholls::http::transport_adapter: Impl does not have transport_delegate's members "
                  "(name, wire_in, app_out, start, established, alpn, shutdown, with their exact types)");

public:
    using implementation_type = std::remove_reference_t<Impl>;

    template <typename... Args, std::enable_if_t<std::is_constructible_v<Impl, Args...>, int> = 0>
    explicit transport_adapter(Args&&... args) noexcept(std::is_nothrow_constructible_v<Impl, Args...>)
        : impl_(std::forward<Args>(args)...)
    {
    }

    implementation_type& implementation() noexcept { return impl_; }
    const implementation_type& implementation() const noexcept { return impl_; }

    const char* name() const noexcept override { return impl_.name(); }
    bool wire_in(const char* data, std::size_t n, std::string& app_in, std::string& wire_out) override
    {
        return impl_.wire_in(data, n, app_in, wire_out);
    }
    bool app_out(const char* data, std::size_t n, std::string& wire_out) override
    {
        return impl_.app_out(data, n, wire_out);
    }
    bool start(std::string& wire_out) override { return impl_.start(wire_out); }
    bool established() const noexcept override { return impl_.established(); }
    const char* alpn() const noexcept override { return impl_.alpn(); }
    void shutdown(std::string& wire_out) override { impl_.shutdown(wire_out); }

private:
    Impl impl_;
};

// Interface to concept: a value that models transport_like by forwarding to a
// transport_delegate it owns (what a transport_factory hands out)
class delegate_transport {
public:
    explicit delegate_transport(std::unique_ptr<transport_delegate> d) noexcept : d_(std::move(d)) {}

    transport_delegate& delegate() noexcept { return *d_; }
    const transport_delegate& delegate() const noexcept { return *d_; }
    std::unique_ptr<transport_delegate> release() noexcept { return std::move(d_); }

    const char* name() const noexcept { return d_->name(); }
    bool wire_in(const char* data, std::size_t n, std::string& app_in, std::string& wire_out)
    {
        return d_->wire_in(data, n, app_in, wire_out);
    }
    bool app_out(const char* data, std::size_t n, std::string& wire_out) { return d_->app_out(data, n, wire_out); }
    bool start(std::string& wire_out) { return d_->start(wire_out); }
    bool established() const noexcept { return d_->established(); }
    const char* alpn() const noexcept { return d_->alpn(); }
    void shutdown(std::string& wire_out) { d_->shutdown(wire_out); }

private:
    std::unique_ptr<transport_delegate> d_;
};

// A transport_factory for a model: each call builds Impl from the hostname and
// the stored arguments (Impl(host, args...) when that compiles, else
// Impl(args...)), wrapped in a transport_adapter
template <typename Impl, typename... Args>
auto make_transport_factory(Args... args)
{
    return [args...](const std::string& host) -> std::unique_ptr<transport_delegate> {
        if constexpr (std::is_constructible_v<Impl, const std::string&, const Args&...>)
            return std::make_unique<transport_adapter<Impl>>(host, args...);
        else
            return std::make_unique<transport_adapter<Impl>>(args...);
    };
}

} // namespace http
} // namespace snicholls

// Each side of the twin matches the other: a member changed on one side only fails here
static_assert(snicholls::http::is_transport_like_v<snicholls::http::transport_delegate>,
              "transport_delegate no longer matches is_transport_like_v in concepts/transport_delegate.hpp");
static_assert(snicholls::http::is_transport_like_v<snicholls::http::plain_transport>);
static_assert(snicholls::http::is_transport_like_v<snicholls::http::delegate_transport>);
#if TS_MOVEABLES_HAS_CONCEPTS
static_assert(snicholls::http::transport_like<snicholls::http::transport_delegate>,
              "transport_delegate no longer matches transport_like in concepts/transport_delegate.hpp");
static_assert(snicholls::http::transport_like<snicholls::http::delegate_transport>);
#endif

#endif /* concepts_transport_delegate_hpp */
