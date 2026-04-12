#include <catch2/catch_test_macros.hpp>

#include "wh/net/concepts/memory_pool_concept.hpp"

namespace {

struct concept_memory_pool {
  auto acquire(const wh::net::memory_acquire_request &) const
      -> wh::net::memory_acquire_result;
  auto acquire(wh::net::memory_acquire_request &&) const
      -> wh::net::memory_acquire_result;
  auto release(const wh::net::memory_block &) const -> wh::net::memory_release_result;
  auto release(wh::net::memory_block &&) const -> wh::net::memory_release_result;
};

struct incomplete_memory_pool {
  auto acquire(const wh::net::memory_acquire_request &) const
      -> wh::net::memory_acquire_result;
};

static_assert(wh::net::memory_pool_like<concept_memory_pool>);
static_assert(!wh::net::memory_pool_like<incomplete_memory_pool>);

} // namespace

TEST_CASE("memory pool concept accepts complete pool adapters",
          "[UT][wh/net/concepts/memory_pool_concept.hpp][memory_pool_like][condition][branch][boundary]") {
  REQUIRE(wh::net::memory_pool_like<concept_memory_pool>);
  REQUIRE_FALSE(wh::net::memory_pool_like<incomplete_memory_pool>);
}

TEST_CASE("memory pool concept rejects adapters missing release handling",
          "[UT][wh/net/concepts/memory_pool_concept.hpp][memory_pool_like][condition][branch][boundary][negative]") {
  constexpr bool valid = wh::net::memory_pool_like<concept_memory_pool>;
  constexpr bool invalid = wh::net::memory_pool_like<incomplete_memory_pool>;
  REQUIRE(valid);
  REQUIRE_FALSE(invalid);
}
