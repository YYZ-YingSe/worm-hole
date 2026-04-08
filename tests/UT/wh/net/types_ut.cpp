#include <catch2/catch_test_macros.hpp>

#include "wh/net/types.hpp"

TEST_CASE("transport services keeps typed host service pointers",
          "[UT][wh/net/types.hpp][transport_services][condition][branch][boundary]") {
  int http = 1;
  int sse = 2;
  int pool = 3;
  int dns = 4;
  int memory = 5;

  wh::net::transport_services<int, int, int, int, int> services{
      .http_client = &http,
      .sse_parser = &sse,
      .connection_pool = &pool,
      .dns_cache = &dns,
      .memory_pool = &memory,
  };

  REQUIRE(*services.http_client == 1);
  REQUIRE(*services.sse_parser == 2);
  REQUIRE(*services.connection_pool == 3);
  REQUIRE(*services.dns_cache == 4);
  REQUIRE(*services.memory_pool == 5);
}

TEST_CASE("transport services default optional pointers are null",
          "[UT][wh/net/types.hpp][transport_services][condition][branch][boundary][default]") {
  wh::net::transport_services<int> services{};
  REQUIRE(services.http_client == nullptr);
  REQUIRE(services.sse_parser == nullptr);
  REQUIRE(services.connection_pool == nullptr);
  REQUIRE(services.dns_cache == nullptr);
  REQUIRE(services.memory_pool == nullptr);
}
