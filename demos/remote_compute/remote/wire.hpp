//
//  remote/wire.hpp
//  TSMoveables - demos/remote_compute (from the remote_compute_example prototype at 92d8dc4)
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Binary serialisation for kernel arguments and results, and the frame that
//  carries them.
//
//  A DEMO, not library API. Aggregate reflection comes from snicholls-core
//  (reflect/struct_tuple.hpp); the serialisation itself belongs in
//  base-encode-decode with the other formats, and moves there when this
//  becomes a library component.
//
//  ------------------------------------------------------------- the encoding
//
//  Field by field, little-endian, never a memcpy of a struct: padding would
//  leak whatever bytes were in it, and two compilers may pad the same struct
//  differently. Integers are written at their own width, floating point as its
//  IEEE bits. Strings and vectors are a u64 count then the elements.
//
//  A type is serialisable if it is (checked in this order):
//    bool, an integer, a floating-point type, an enum;
//    std::string, std::vector<T> of a serialisable T;
//    tuple-like - std::tuple_size<T> plus get<I> found by ADL - which covers
//      std::pair, std::tuple, std::array and crtp objects, whose tuple protocol
//      is already there (crtp/tuple_protocol.hpp);
//    a plain aggregate (not a union) of serialisable members, up to
//      SNICHOLLS_MAX_STRUCT_MEMBERS (256).
//
//  ---------------------------------------------------------------- identity
//
//  signature_v<T> is a 64-bit hash of the STRUCTURE of T - its kinds, widths
//  and members, recursively - not of its name and not of __PRETTY_FUNCTION__,
//  which differs between Clang on a Mac and GCC on a Pi. Two builds that agree
//  on the hash agree on the bytes. A long that is 64 bits on one machine and
//  32 on another hashes differently, so the call is refused rather than
//  misread. It is the role crtp::shape_hash_v plays for crtp's envelopes.
//
//  ------------------------------------------------------------------ frames
//
//  One frame per message, in crtp's envelope layout, so crtp::parse_envelope
//  reads these frames unchanged:
//
//      crtp1 <kind>:<kernel> binary <signature hex16> <length>\n<payload>
//
//  kind is call, ret, err or hello, plus prog (a running job's progress),
//  ping and pong (health), cancel, ack (a result received), and the handshake:
//  hello, auth, welcome, and bye (refused, or shutting down). The payload of
//  call, ret, err, prog, cancel and ack starts with the u64 call id.
//

#ifndef remote_wire_hpp
#define remote_wire_hpp

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <snicholls/core/reflect/struct_tuple.hpp>

namespace snicholls::remote {

/// Thrown when bytes cannot be decoded: truncated, oversized, or not a frame.
struct wire_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ===================================================================== reflection
// Plain aggregates are bound by snicholls-core: snicholls::tie (reflect/
// struct_tuple.hpp) is a std::tuple of references to the members, from
// generated binders up to SNICHOLLS_MAX_STRUCT_MEMBERS (256). Its
// plain_aggregate also refuses unions, which a wire format must.

namespace detail {

using snicholls::plain_aggregate;

template <typename T>
constexpr auto tie_members(T& v) noexcept
{
    return snicholls::tie(v);
}

template <typename T>
struct is_vector : std::false_type {};
template <typename T, typename A>
struct is_vector<std::vector<T, A>> : std::true_type {};

template <typename T>
concept tuple_like = requires { std::tuple_size<T>::value; };

// get<I> found by ADL, so crtp::get is found for crtp objects (crtp's rule)
template <std::size_t I, typename T>
constexpr decltype(auto) adl_get(T&& t)
{
    using std::get;
    return get<I>(std::forward<T>(t));
}

} // namespace detail

// ====================================================================== identity

namespace detail {

constexpr std::uint64_t mix(std::uint64_t h, std::uint64_t v) noexcept
{
    // FNV-1a over the 8 bytes of v: simple, stable, and the same everywhere
    for (int i = 0; i < 8; ++i) {
        h ^= (v >> (8 * i)) & 0xffu;
        h *= 0x100000001b3ull;
    }
    return h;
}

constexpr std::uint64_t fnv_basis = 0xcbf29ce484222325ull;

enum class kind : std::uint64_t {
    boolean = 1, sint, uint, floating, string, vector, tuple, aggregate, enumeration, function, void_type
};

template <typename T>
constexpr std::uint64_t signature();

template <typename Tuple, std::size_t... I>
constexpr std::uint64_t signature_of_elements(std::uint64_t h, std::index_sequence<I...>)
{
    ((h = mix(h, signature<std::remove_cvref_t<std::tuple_element_t<I, Tuple>>>())), ...);
    return h;
}

template <typename T>
constexpr std::uint64_t signature()
{
    using U = std::remove_cv_t<T>;
    if constexpr (std::is_void_v<U>)
        return mix(fnv_basis, std::uint64_t(kind::void_type));
    else if constexpr (std::is_same_v<U, bool>)
        return mix(fnv_basis, std::uint64_t(kind::boolean));
    else if constexpr (std::is_enum_v<U>)
        return mix(mix(fnv_basis, std::uint64_t(kind::enumeration)), signature<std::underlying_type_t<U>>());
    else if constexpr (std::is_integral_v<U>)
        return mix(mix(fnv_basis, std::uint64_t(std::is_signed_v<U> ? kind::sint : kind::uint)), sizeof(U));
    else if constexpr (std::is_floating_point_v<U>)
        return mix(mix(fnv_basis, std::uint64_t(kind::floating)), sizeof(U));
    else if constexpr (std::is_same_v<U, std::string>)
        return mix(fnv_basis, std::uint64_t(kind::string));
    else if constexpr (is_vector<U>::value)
        return mix(mix(fnv_basis, std::uint64_t(kind::vector)), signature<typename U::value_type>());
    else if constexpr (tuple_like<U>) {
        constexpr std::size_t n = std::tuple_size_v<U>;
        return signature_of_elements<U>(mix(mix(fnv_basis, std::uint64_t(kind::tuple)), n),
                                        std::make_index_sequence<n>{});
    }
    else if constexpr (plain_aggregate<U>) {
        using tied = decltype(tie_members(std::declval<U&>()));
        constexpr std::size_t n = std::tuple_size_v<tied>;
        return signature_of_elements<tied>(mix(mix(fnv_basis, std::uint64_t(kind::aggregate)), n),
                                           std::make_index_sequence<n>{});
    }
    else
        static_assert(sizeof(U) == 0, "not wire-serialisable: see remote/wire.hpp for what is");
}

} // namespace detail

/// The structural hash of T. Equal on two machines exactly when the bytes mean the same.
template <typename T>
inline constexpr std::uint64_t signature_v = detail::signature<T>();

/// The identity of a function signature R(Args...), arguments decayed.
template <typename R, typename... Args>
inline constexpr std::uint64_t function_signature_v = [] {
    std::uint64_t h = detail::mix(detail::fnv_basis, std::uint64_t(detail::kind::function));
    h = detail::mix(h, signature_v<R>);
    ((h = detail::mix(h, signature_v<std::remove_cvref_t<Args>>)), ...);
    return h;
}();

// ===================================================================== encoding

class writer {
public:
    explicit writer(std::string& out) : out_(out) {}

    template <typename T>
    void put(const T& v)
    {
        using U = std::remove_cv_t<T>;
        if constexpr (std::is_same_v<U, bool>)
            out_.push_back(v ? '\1' : '\0');
        else if constexpr (std::is_enum_v<U>)
            put(static_cast<std::underlying_type_t<U>>(v));
        else if constexpr (std::is_integral_v<U>)
            put_le(static_cast<std::make_unsigned_t<U>>(v));
        else if constexpr (std::is_same_v<U, float>)
            put_le(std::bit_cast<std::uint32_t>(v));
        else if constexpr (std::is_same_v<U, double>)
            put_le(std::bit_cast<std::uint64_t>(v));
        else if constexpr (std::is_same_v<U, std::string>) {
            put_le(std::uint64_t(v.size()));
            out_.append(v);
        }
        else if constexpr (detail::is_vector<U>::value) {
            put_le(std::uint64_t(v.size()));
            for (const auto& e : v)
                put(e);
        }
        else if constexpr (detail::tuple_like<U>)
            put_elements(v, std::make_index_sequence<std::tuple_size_v<U>>{});
        else if constexpr (detail::plain_aggregate<U>)
            std::apply([&](const auto&... m) { (put(m), ...); }, detail::tie_members(v));
        else
            static_assert(sizeof(U) == 0, "not wire-serialisable");
    }

private:
    template <typename U>
    void put_le(U v)
    {
        for (std::size_t i = 0; i < sizeof(U); ++i)
            out_.push_back(static_cast<char>((v >> (8 * i)) & 0xffu));
    }

    template <typename T, std::size_t... I>
    void put_elements(const T& v, std::index_sequence<I...>)
    {
        (put(detail::adl_get<I>(v)), ...);
    }

    std::string& out_;
};

class reader {
public:
    explicit reader(std::string_view in) : in_(in) {}

    std::size_t remaining() const noexcept { return in_.size() - pos_; }

    template <typename T>
    void get(T& v)
    {
        using U = std::remove_cv_t<T>;
        if constexpr (std::is_same_v<U, bool>) {
            const auto b = take(1)[0];
            if (b != '\0' && b != '\1')
                throw wire_error("wire: a bool that is neither 0 nor 1");
            v = b == '\1';
        }
        else if constexpr (std::is_enum_v<U>) {
            std::underlying_type_t<U> raw{};
            get(raw);
            v = static_cast<U>(raw);
        }
        else if constexpr (std::is_integral_v<U>)
            v = static_cast<U>(get_le<std::make_unsigned_t<U>>());
        else if constexpr (std::is_same_v<U, float>)
            v = std::bit_cast<float>(get_le<std::uint32_t>());
        else if constexpr (std::is_same_v<U, double>)
            v = std::bit_cast<double>(get_le<std::uint64_t>());
        else if constexpr (std::is_same_v<U, std::string>) {
            const auto n = count(1);
            const auto bytes = take(n);
            v.assign(bytes.data(), bytes.size());
        }
        else if constexpr (detail::is_vector<U>::value) {
            // Every element is at least one byte, so a count larger than what is
            // left is a lie, refused before it becomes a huge allocation
            const auto n = count(1);
            v.clear();
            v.resize(n);
            for (auto& e : v)
                get(e);
        }
        else if constexpr (detail::tuple_like<U>)
            get_elements(v, std::make_index_sequence<std::tuple_size_v<U>>{});
        else if constexpr (detail::plain_aggregate<U>)
            std::apply([&](auto&... m) { (get(m), ...); }, detail::tie_members(v));
        else
            static_assert(sizeof(U) == 0, "not wire-serialisable");
    }

    template <typename T>
    T get()
    {
        T v{};
        get(v);
        return v;
    }

    void expect_end() const
    {
        if (pos_ != in_.size())
            throw wire_error("wire: trailing bytes after the last value");
    }

private:
    std::string_view take(std::size_t n)
    {
        if (n > remaining())
            throw wire_error("wire: truncated");
        const auto s = in_.substr(pos_, n);
        pos_ += n;
        return s;
    }

    template <typename U>
    U get_le()
    {
        const auto b = take(sizeof(U));
        U v = 0;
        for (std::size_t i = 0; i < sizeof(U); ++i)
            v |= static_cast<U>(static_cast<unsigned char>(b[i])) << (8 * i);
        return v;
    }

    std::size_t count(std::size_t min_element_bytes)
    {
        const auto n = get_le<std::uint64_t>();
        if (n > remaining() / min_element_bytes)
            throw wire_error("wire: a count larger than the bytes that follow");
        return static_cast<std::size_t>(n);
    }

    template <typename T, std::size_t... I>
    void get_elements(T& v, std::index_sequence<I...>)
    {
        (get(detail::adl_get<I>(v)), ...);
    }

    std::string_view in_;
    std::size_t pos_ = 0;
};

/// Encode values in order.
template <typename... T>
void encode(std::string& out, const T&... v)
{
    writer w(out);
    (w.put(v), ...);
}

// ======================================================================= frames

enum class frame_kind { call, ret, err, hello, prog, ping, pong, cancel, auth, welcome, ack, bye };

struct frame {
    frame_kind kind{};
    std::string_view kernel;     // the kernel name (call, ret, err) or node name (hello)
    std::uint64_t signature = 0;
    std::string_view payload;
};

inline constexpr std::size_t max_payload = std::size_t{64} << 20;

inline std::string_view kind_name(frame_kind k)
{
    switch (k) {
    case frame_kind::call: return "call";
    case frame_kind::ret: return "ret";
    case frame_kind::err: return "err";
    case frame_kind::hello: return "hello";
    case frame_kind::prog: return "prog";
    case frame_kind::ping: return "ping";
    case frame_kind::pong: return "pong";
    case frame_kind::cancel: return "cancel";
    case frame_kind::auth: return "auth";
    case frame_kind::welcome: return "welcome";
    case frame_kind::ack: return "ack";
    case frame_kind::bye: return "bye";
    }
    return "?";
}

/// Append one frame in crtp's envelope layout.
inline void append_frame(std::string& out, frame_kind kind, std::string_view kernel, std::uint64_t sig,
                         std::string_view payload)
{
    out += "crtp1 ";
    out += kind_name(kind);
    out += ':';
    out += kernel;
    out += " binary ";
    constexpr char hex[] = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4)
        out += hex[(sig >> shift) & 0xf];
    out += ' ';
    out += std::to_string(payload.size());
    out += '\n';
    out.append(payload);
}

/// Parse one frame from the front of buf. Returns the bytes consumed, 0 when
/// more bytes are needed, and throws wire_error on anything malformed.
inline std::size_t parse_frame(std::string_view buf, frame& f)
{
    constexpr std::string_view magic = "crtp1 ";
    if (buf.substr(0, std::min(buf.size(), magic.size())) != magic.substr(0, std::min(buf.size(), magic.size())))
        throw wire_error("frame: missing crtp1 magic");
    const auto nl = buf.find('\n');
    if (nl == std::string_view::npos) {
        if (buf.size() > 512)
            throw wire_error("frame: header too long");
        return 0;
    }
    std::string_view h = buf.substr(magic.size(), nl - magic.size());
    auto field = [&h] {
        const auto sp = h.find(' ');
        const auto s = h.substr(0, sp);
        h = sp == std::string_view::npos ? std::string_view{} : h.substr(sp + 1);
        return s;
    };
    const auto type = field(), format = field(), sig = field(), len = field();
    if (!h.empty() || format != "binary" || sig.size() != 16 || len.empty())
        throw wire_error("frame: malformed header");
    const auto colon = type.find(':');
    if (colon == std::string_view::npos)
        throw wire_error("frame: type has no kind");
    const auto k = type.substr(0, colon);
    if (k == "call") f.kind = frame_kind::call;
    else if (k == "ret") f.kind = frame_kind::ret;
    else if (k == "err") f.kind = frame_kind::err;
    else if (k == "hello") f.kind = frame_kind::hello;
    else if (k == "prog") f.kind = frame_kind::prog;
    else if (k == "ping") f.kind = frame_kind::ping;
    else if (k == "pong") f.kind = frame_kind::pong;
    else if (k == "cancel") f.kind = frame_kind::cancel;
    else if (k == "auth") f.kind = frame_kind::auth;
    else if (k == "welcome") f.kind = frame_kind::welcome;
    else if (k == "ack") f.kind = frame_kind::ack;
    else if (k == "bye") f.kind = frame_kind::bye;
    else throw wire_error("frame: unknown kind");
    f.kernel = type.substr(colon + 1);
    f.signature = 0;
    for (char c : sig) {
        const bool d = c >= '0' && c <= '9';
        if (!d && !(c >= 'a' && c <= 'f'))
            throw wire_error("frame: signature is not lowercase hex");
        f.signature = (f.signature << 4) | std::uint64_t(d ? c - '0' : c - 'a' + 10);
    }
    std::uint64_t n = 0;
    for (char c : len) {
        if (c < '0' || c > '9' || n > max_payload)
            throw wire_error("frame: bad length");
        n = n * 10 + std::uint64_t(c - '0');
    }
    if (n > max_payload)
        throw wire_error("frame: payload too large");
    const std::size_t header = nl + 1;
    if (buf.size() - header < n)
        return 0;
    f.payload = buf.substr(header, static_cast<std::size_t>(n));
    return header + static_cast<std::size_t>(n);
}

} // namespace snicholls::remote

#endif /* remote_wire_hpp */
