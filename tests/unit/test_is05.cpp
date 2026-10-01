#include "doctest/doctest.h"

#include "nmos/is05.hpp"

TEST_CASE("is-05 accepts uuids and nulls")
{
    mwm::Is05Activation activation;
    auto const err = mwm::validateIs05(R"({
        "sender_id": "11111111-1111-4111-8111-111111111111",
        "master_enable": true,
        "activation": {"mode": "activate_immediate"},
        "transport_params": [{
            "mxl_domain_id": "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
            "mxl_flow_id": "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"
        }]
    })",
        &activation);
    CHECK_FALSE(err.has_value());
    CHECK(activation.master_enable);
    CHECK(activation.domain_id == "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    CHECK(activation.flow_id == "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
}

TEST_CASE("is-05 rejects malformed ids only")
{
    auto const bad = mwm::validateIs05(R"({"transport_params":[{"mxl_flow_id":"not-a-uuid"}]})", nullptr);
    REQUIRE(bad.has_value());
    CHECK(bad->status == 400);
    auto const domain = mwm::validateIs05(R"({"transport_params":[{"mxl_domain_id":"auto"}]})", nullptr);
    REQUIRE(domain.has_value());
    mwm::Is05Activation activation;
    auto const missing = mwm::validateIs05(R"({"master_enable":true,"transport_params":[{"mxl_domain_id":null,"mxl_flow_id":null}]})", &activation);
    CHECK_FALSE(missing.has_value());
    CHECK(activation.master_enable);
    CHECK(activation.flow_id.empty());
}
