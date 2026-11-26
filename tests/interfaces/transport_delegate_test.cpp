//
//  transport_delegate_test.cpp (interfaces)
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - the interface form of transport_delegate (interfaces/transport_delegate.hpp) and the
//  adapters between the forms: plain_transport through the conformance kit as itself and as transport_delegate&, a
//  model through transport_adapter (by value and as a view) and make_transport_factory, and a transport_delegate
//  through delegate_transport. The TLS backends run through the same kit in make test-tls.
//

#include "../test_helpers.hpp"

#include <memory>

#include "../support/transport_models.hpp"

namespace {

using namespace transport_tests;

void test_plain_in_both_forms()
{
    plain_transport c, s;
    auto f = check_transport_pair(c, s, {""});
    print(f);
    assert(f.empty());

    // The same pair held as the interface, which models the concept too
    plain_transport c2, s2;
    transport_delegate& ci = c2;
    transport_delegate& si = s2;
    f = check_transport_pair(ci, si);
    print(f);
    assert(f.empty());
    pass("transport kit: plain_transport passes, as itself and through transport_delegate&");
}

void test_model_through_adapters()
{
    xor_transport c(true), s(false);
    auto f = check_transport_pair(c, s, {"xor/1"});
    print(f);
    assert(f.empty());

    // Concept to interface, by value and bound to a model by reference
    transport_adapter<xor_transport> ca(true), sa(false);
    transport_delegate& ci = ca;
    transport_delegate& si = sa;
    f = check_transport_pair(ci, si, {"xor/1"});
    print(f);
    assert(f.empty());
    xor_transport rc(true), rs(false);
    transport_adapter<xor_transport&> view_c(rc), view_s(rs);
    f = check_transport_pair(view_c, view_s, {"xor/1"});
    print(f);
    assert(f.empty() && rc.established() && &view_c.implementation() == &rc);

    // A factory of them, the shape every outbound connection takes; the hostname reaches the model
    auto factory = make_transport_factory<xor_transport>(true);
    std::unique_ptr<transport_delegate> made = factory("example.com");
    assert(std::string(made->name()) == "xor");
    assert(static_cast<transport_adapter<xor_transport>&>(*made).implementation().host() == "example.com");

    // Interface to concept: what a factory hands out, as a value for templates over transport_like
    delegate_transport dc(std::move(made));
    delegate_transport ds(std::make_unique<transport_adapter<xor_transport>>(false));
    f = check_transport_pair(dc, ds, {"xor/1"});
    print(f);
    assert(f.empty());

    pass("transport kit: a no-vtable model passes through transport_adapter (value and view), a factory "
         "and delegate_transport");
}

} // namespace

void run_transport_interface_tests()
{
    test_plain_in_both_forms();
    test_model_through_adapters();
}
