#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <span>

#include "wh/net/types/memory_pool_types.hpp"

TEST_CASE("memory pool types expose default alignment and byte spans",
          "[UT][wh/net/types/memory_pool_types.hpp][memory_acquire_request][condition][branch][boundary]") {
  wh::net::memory_acquire_request request{};
  REQUIRE(request.size_bytes == 0U);
  REQUIRE(request.alignment == alignof(std::max_align_t));

  std::array<std::byte, 4> bytes{};
  wh::net::memory_block block{};
  block.bytes = std::span<std::byte>{bytes.data(), bytes.size()};
  REQUIRE(block.bytes.size() == 4U);
}

TEST_CASE("memory pool types default blocks can represent empty spans",
          "[UT][wh/net/types/memory_pool_types.hpp][memory_block][condition][branch][boundary]") {
  wh::net::memory_block block{};
  REQUIRE(block.bytes.empty());

  wh::net::memory_acquire_request request{};
  request.size_bytes = 64U;
  request.alignment = 16U;
  REQUIRE(request.size_bytes == 64U);
  REQUIRE(request.alignment == 16U);
}
