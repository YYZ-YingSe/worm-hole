#include <catch2/catch_test_macros.hpp>

#include "wh/net/types/connection_pool_types.hpp"

TEST_CASE("connection pool types project owned keys into borrowed views",
          "[UT][wh/net/types/"
          "connection_pool_types.hpp][make_connection_key_view][condition][branch][boundary]") {
  wh::net::connection_key key{};
  key.scheme = "https";
  key.host = "example.com";
  key.port = 443U;

  const auto view = wh::net::make_connection_key_view(key);
  REQUIRE(view.scheme == "https");
  REQUIRE(view.host == "example.com");
  REQUIRE(view.port == 443U);
}

TEST_CASE("connection pool types default lease and release metadata stay stable",
          "[UT][wh/net/types/"
          "connection_pool_types.hpp][connection_release][condition][branch][boundary]") {
  wh::net::connection_lease lease{};
  REQUIRE(lease.lease_id.empty());
  REQUIRE_FALSE(lease.reused);

  wh::net::connection_release release{};
  release.lease = std::move(lease);
  REQUIRE(release.reusable);
  REQUIRE(release.lease.key.port == 0U);
}
