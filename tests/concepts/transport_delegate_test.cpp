//
//  transport_delegate_test.cpp (concepts)
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - the concept form of transport_delegate (concepts/transport_delegate.hpp): the trait and
//  the concept accept the interface, plain_transport and a no-vtable model, and refuse members that are nearly right;
//  the conformance kit passes a model as itself and catches a transport that drops bytes, plaintext garbage and the
//  wrong ALPN.
//

#include "../test_helpers.hpp"

#include "../support/transport_models.hpp"

namespace {

using namespace transport_tests;

static_assert(is_transport_like_v<xor_transport> && is_transport_like_v<plain_transport> &&
              is_transport_like_v<transport_delegate>);
static_assert(!is_transport_like_v<not_const_name> && !is_transport_like_v<throwing_alpn> && !is_transport_like_v<int>);
#if TS_MOVEABLES_HAS_CONCEPTS
static_assert(transport_like<xor_transport> && transport_like<plain_transport> && !transport_like<throwing_alpn>);
#endif

void test_a_model_as_itself()
{
    xor_transport c(true), s(false);
    auto f = check_transport_pair(c, s, {"xor/1"});
    print(f);
    assert(f.empty());
    xor_transport fresh(false);
    f = check_rejects_garbage(fresh);
    print(f);
    assert(f.empty());
    pass("transport concept: a no-vtable model passes the kit as itself, and refuses garbage");
}

void test_kit_catches_a_broken_transport()
{
    lossy_transport c, s;
    const auto f = check_transport_pair(c, s);
    assert(f.size() >= 8);                       // every payload, both directions
    plain_transport p;
    assert(!check_rejects_garbage(p).empty());   // plaintext surfaces garbage: right for plain, wrong for TLS
    xor_transport wrong_alpn_c(true), wrong_alpn_s(false);
    assert(check_transport_pair(wrong_alpn_c, wrong_alpn_s, {"h2"}).size() == 1);
    pass("transport kit: a transport that drops bytes, plaintext garbage and the wrong ALPN are all reported");
}

} // namespace

void run_transport_concept_tests()
{
    test_a_model_as_itself();
    test_kit_catches_a_broken_transport();
}
