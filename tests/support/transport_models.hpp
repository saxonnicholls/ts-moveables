//
//  transport_models.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Test models of transport_like shared by tests/concepts/transport_delegate_test.cpp and
//  tests/interfaces/transport_delegate_test.cpp: a working one with no vtable, two that are nearly right (the trait
//  must refuse them), and one that loses bytes (the conformance kit must catch it).
//

#ifndef ts_moveables_tests_transport_models_hpp
#define ts_moveables_tests_transport_models_hpp

#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

#include "../../TSMoveables/concepts/transport_delegate.hpp"
#include "../../TSMoveables/interfaces/transport_delegate_conformance.hpp"

namespace transport_tests {

using namespace snicholls::http;

// A model of transport_like with no vtable that protects something, so every check in the kit has work to do:
// the client says "HELLO", the server answers "OK", and after that every byte is XORed with a key. The hostname
// it is built from is kept, so make_transport_factory's (host, args...) form is exercised.
class xor_transport {
public:
    explicit xor_transport(bool client) : client_(client) {}
    xor_transport(std::string host, bool client) : client_(client), host_(std::move(host)) {}   // the factory's form

    const char* name() const noexcept { return "xor"; }
    bool start(std::string& wire_out)
    {
        if (client_)
            wire_out += "HELLO";
        return true;
    }
    bool wire_in(const char* data, std::size_t n, std::string& app_in, std::string& wire_out)
    {
        for (std::size_t i = 0; i < n; ++i) {
            if (!established_) {
                hs_.push_back(data[i]);
                const std::string want = client_ ? "OK" : "HELLO";
                if (want.compare(0, hs_.size(), hs_) != 0)
                    return false;                       // not our handshake: fatal
                if (hs_ == want) {
                    established_ = true;
                    if (!client_)
                        wire_out += "OK";
                    wire_out += pending_;               // what was queued before the handshake ended
                    pending_.clear();
                }
                continue;
            }
            app_in.push_back(char(data[i] ^ key));
        }
        return true;
    }
    bool app_out(const char* data, std::size_t n, std::string& wire_out)
    {
        std::string& to = established_ ? wire_out : pending_;
        for (std::size_t i = 0; i < n; ++i)
            to.push_back(char(data[i] ^ key));
        return true;
    }
    bool established() const noexcept { return established_; }
    const char* alpn() const noexcept { return "xor/1"; }
    void shutdown(std::string&) {}
    const std::string& host() const noexcept { return host_; }

private:
    static constexpr char key = 0x5a;
    bool client_;
    std::string host_;
    std::string hs_, pending_;
    bool established_ = false;
};

static_assert(is_transport_like_v<xor_transport>);
#if TS_MOVEABLES_HAS_CONCEPTS
static_assert(transport_like<xor_transport>);
#endif

// Members that are nearly right are not right: the trait refuses a wrong const, a missing noexcept, a wrong type
struct not_const_name {
    const char* name() noexcept { return "x"; }
    bool wire_in(const char*, std::size_t, std::string&, std::string&) { return true; }
    bool app_out(const char*, std::size_t, std::string&) { return true; }
    bool start(std::string&) { return true; }
    bool established() const noexcept { return true; }
    const char* alpn() const noexcept { return ""; }
    void shutdown(std::string&) {}
};
struct throwing_alpn {
    const char* name() const noexcept { return "x"; }
    bool wire_in(const char*, std::size_t, std::string&, std::string&) { return true; }
    bool app_out(const char*, std::size_t, std::string&) { return true; }
    bool start(std::string&) { return true; }
    bool established() const noexcept { return true; }
    const char* alpn() const { return ""; }
    void shutdown(std::string&) {}
};
static_assert(!is_transport_like_v<not_const_name> && !is_transport_like_v<throwing_alpn> && !is_transport_like_v<int>);

// Loses the last byte of every write: the kit must say so
class lossy_transport {
public:
    const char* name() const noexcept { return "lossy"; }
    bool start(std::string&) { return true; }
    bool wire_in(const char* data, std::size_t n, std::string& app_in, std::string&)
    {
        app_in.append(data, n);
        return true;
    }
    bool app_out(const char* data, std::size_t n, std::string& wire_out)
    {
        wire_out.append(data, n ? n - 1 : 0);
        return true;
    }
    bool established() const noexcept { return true; }
    const char* alpn() const noexcept { return ""; }
    void shutdown(std::string&) {}
};

inline void print(const std::vector<std::string>& failures)
{
    for (const auto& f : failures) std::cerr << "  conformance: " << f << "\n";
}

} // namespace transport_tests

#endif // ts_moveables_tests_transport_models_hpp
