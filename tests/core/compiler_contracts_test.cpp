#include <catch2/catch_test_macros.hpp>

#include "wh/core/compiler.hpp"

TEST_CASE("compiler alignment and power_of_two contracts", "[core][compiler][condition]") {
  REQUIRE(wh::core::is_power_of_two(1));
  REQUIRE(wh::core::is_power_of_two(2));
  REQUIRE_FALSE(wh::core::is_power_of_two(0));
  REQUIRE_FALSE(wh::core::is_power_of_two(3));

  REQUIRE(wh::core::align_up(10, 8) == 16);
  REQUIRE(wh::core::align_up(16, 8) == 16);
  REQUIRE(wh::core::align_up(9, 3) == 9);
}

TEST_CASE("compiler branch prediction helpers keep boolean semantics", "[core][compiler][branch]") {
  REQUIRE(wh::core::predict_likely(true));
  REQUIRE_FALSE(wh::core::predict_likely(false));
  REQUIRE(wh::core::predict_unlikely(true));
  REQUIRE_FALSE(wh::core::predict_unlikely(false));
}

TEST_CASE("compiler next_power_of_two handles edge values", "[core][compiler][extreme]") {
  REQUIRE(wh::core::next_power_of_two(0) == 1);
  REQUIRE(wh::core::next_power_of_two(1) == 1);
  REQUIRE(wh::core::next_power_of_two(1025) == 2048);

  constexpr std::size_t max_shiftable =
      static_cast<std::size_t>(1) << ((sizeof(std::size_t) * 8U) - 2U);
  REQUIRE(wh::core::next_power_of_two(max_shiftable) == max_shiftable);
}
