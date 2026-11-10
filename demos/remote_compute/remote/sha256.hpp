//
//  remote/sha256.hpp
//  TSMoveables - demos/remote_compute (from the remote_compute_example prototype at 92d8dc4)
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  SHA-256 (FIPS 180-4) and HMAC-SHA256 (RFC 2104), for authenticating
//  machines to each other with a shared key. Dependency-free, because
//  burrawang has no OpenSSL and neither will a fresh Raspberry Pi.
//
//  PROTOTYPE COPY. SHA-256 belongs in base-encode-decode (the agreed home for
//  byte-level hashing); this file is replaced by that one when it lands. It is
//  checked against the FIPS 180-4 and RFC 4231 test vectors in the self-test.
//
//  Authentication only. Nothing here encrypts: on the home LAN the threat is
//  someone running kernels they should not, which a shared key stops. Privacy
//  of the traffic itself needs TLS, which needs OpenSSL on every machine.
//

#ifndef remote_sha256_hpp
#define remote_sha256_hpp

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>

namespace snicholls::remote {

using digest = std::array<unsigned char, 32>;

class sha256 {
public:
    sha256() { reset(); }

    void reset()
    {
        h_ = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        len_ = 0;
        used_ = 0;
    }

    sha256& update(std::string_view data)
    {
        for (const char c : data) {
            block_[used_++] = static_cast<unsigned char>(c);
            if (used_ == 64) {
                compress();
                used_ = 0;
            }
        }
        len_ += data.size();
        return *this;
    }

    digest finish()
    {
        const std::uint64_t bits = len_ * 8;
        block_[used_++] = 0x80;
        if (used_ > 56) {
            while (used_ < 64)
                block_[used_++] = 0;
            compress();
            used_ = 0;
        }
        while (used_ < 56)
            block_[used_++] = 0;
        for (int i = 7; i >= 0; --i)
            block_[used_++] = static_cast<unsigned char>(bits >> (8 * i));
        compress();
        digest out{};
        for (std::size_t i = 0; i < 8; ++i)
            for (std::size_t b = 0; b < 4; ++b)
                out[i * 4 + b] = static_cast<unsigned char>(h_[i] >> (24 - 8 * b));
        reset();
        return out;
    }

    static digest of(std::string_view data) { return sha256().update(data).finish(); }

private:
    static constexpr std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void compress()
    {
        static constexpr std::uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        std::uint32_t w[64];
        for (std::size_t i = 0; i < 16; ++i)
            w[i] = std::uint32_t(block_[i * 4]) << 24 | std::uint32_t(block_[i * 4 + 1]) << 16 |
                   std::uint32_t(block_[i * 4 + 2]) << 8 | std::uint32_t(block_[i * 4 + 3]);
        for (std::size_t i = 16; i < 64; ++i) {
            const auto s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const auto s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (std::size_t i = 0; i < 64; ++i) {
            const auto t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const auto t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d, h_[4] += e, h_[5] += f, h_[6] += g, h_[7] += h;
    }

    std::array<std::uint32_t, 8> h_{};
    std::array<unsigned char, 64> block_{};
    std::uint64_t len_ = 0;
    std::size_t used_ = 0;
};

inline std::string_view as_view(const digest& d)
{
    return {reinterpret_cast<const char*>(d.data()), d.size()};
}

/// HMAC-SHA256 (RFC 2104).
inline digest hmac_sha256(std::string_view key, std::string_view message)
{
    std::array<unsigned char, 64> k{};
    if (key.size() > 64) {
        const auto hk = sha256::of(key);
        for (std::size_t i = 0; i < hk.size(); ++i)
            k[i] = hk[i];
    }
    else
        for (std::size_t i = 0; i < key.size(); ++i)
            k[i] = static_cast<unsigned char>(key[i]);
    std::string ipad(64, '\0'), opad(64, '\0');
    for (std::size_t i = 0; i < 64; ++i) {
        ipad[i] = static_cast<char>(k[i] ^ 0x36);
        opad[i] = static_cast<char>(k[i] ^ 0x5c);
    }
    const auto inner = sha256().update(ipad).update(message).finish();
    return sha256().update(opad).update(as_view(inner)).finish();
}

/// Compare without an early exit, so the time taken says nothing about where
/// a forged MAC first differs.
inline bool equal_constant_time(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

inline std::string to_hex(std::string_view bytes)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const char c : bytes) {
        out += hex[(static_cast<unsigned char>(c) >> 4) & 0xf];
        out += hex[static_cast<unsigned char>(c) & 0xf];
    }
    return out;
}

inline std::string from_hex(std::string_view hex)
{
    if (hex.size() % 2 != 0)
        throw std::invalid_argument("hex: odd length");
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        throw std::invalid_argument("hex: not a hex digit");
    };
    std::string out(hex.size() / 2, '\0');
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<char>(nibble(hex[2 * i]) << 4 | nibble(hex[2 * i + 1]));
    return out;
}

/// Cryptographically random bytes from the operating system.
inline std::string random_bytes(std::size_t n)
{
    std::string out(n, '\0');
    std::FILE* f = std::fopen("/dev/urandom", "rb");
    if (f == nullptr || std::fread(out.data(), 1, n, f) != n) {
        if (f != nullptr)
            std::fclose(f);
        throw std::runtime_error("cannot read /dev/urandom");
    }
    std::fclose(f);
    return out;
}

} // namespace snicholls::remote

#endif /* remote_sha256_hpp */
