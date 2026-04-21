#include <catch2/catch_test_macros.hpp>

#include "wh/net/concepts/connection_pool_concept.hpp"

namespace {

struct concept_connection_pool {
  auto acquire(const wh::net::connection_key &) const -> wh::net::connection_acquire_result;
  auto acquire(wh::net::connection_key &&) const -> wh::net::connection_acquire_result;
  auto acquire(const wh::net::connection_key_view) const -> wh::net::connection_acquire_result;
  auto release(const wh::net::connection_release &) const -> wh::net::connection_release_result;
  auto release(wh::net::connection_release &&) const -> wh::net::connection_release_result;
};

struct incomplete_connection_pool {
  auto acquire(const wh::net::connection_key &) const -> wh::net::connection_acquire_result;
};

static_assert(wh::net::connection_pool_like<concept_connection_pool>);
static_assert(!wh::net::connection_pool_like<incomplete_connection_pool>);

} // namespace

TEST_CASE("connection pool concept accepts complete pool adapters",
          "[UT][wh/net/concepts/"
          "connection_pool_concept.hpp][connection_pool_like][condition][branch][boundary]") {
  REQUIRE(wh::net::connection_pool_like<concept_connection_pool>);
  REQUIRE_FALSE(wh::net::connection_pool_like<incomplete_connection_pool>);
}

TEST_CASE(
    "connection pool concept distinguishes missing release surface",
    "[UT][wh/net/concepts/"
    "connection_pool_concept.hpp][connection_pool_like][condition][branch][boundary][negative]") {
  constexpr bool valid = wh::net::connection_pool_like<concept_connection_pool>;
  constexpr bool invalid = wh::net::connection_pool_like<incomplete_connection_pool>;
  REQUIRE(valid);
  REQUIRE_FALSE(invalid);
}
