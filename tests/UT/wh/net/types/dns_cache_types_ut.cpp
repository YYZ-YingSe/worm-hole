#include <chrono>

#include <catch2/catch_test_macros.hpp>

#include "wh/net/types/dns_cache_types.hpp"

TEST_CASE("dns cache types project owned requests into borrowed views",
          "[UT][wh/net/types/"
          "dns_cache_types.hpp][make_dns_lookup_request_view][condition][branch][boundary]") {
  wh::net::dns_lookup_request request{};
  request.host = "example.com";
  request.port = 443U;

  const auto view = wh::net::make_dns_lookup_request_view(request);
  REQUIRE(view.host == "example.com");
  REQUIRE(view.port == 443U);
}

TEST_CASE(
    "dns cache types default response and records keep zeroed state",
    "[UT][wh/net/types/dns_cache_types.hpp][dns_lookup_response][condition][branch][boundary]") {
  wh::net::dns_record record{};
  REQUIRE(record.address.empty());
  REQUIRE(record.port == 0U);
  REQUIRE(record.ttl == std::chrono::seconds{0});

  wh::net::dns_lookup_response response{};
  REQUIRE(response.records.empty());
  REQUIRE_FALSE(response.cache_hit);
}
