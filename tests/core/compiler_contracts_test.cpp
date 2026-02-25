#include <catch2/catch_test_macros.hpp>

#include "wh/core/compiler.hpp"

TEST_CASE("compiler alignment and power_of_two contracts",
          "[core][compiler][condition]") {
  REQUIRE(wh::core::is_power_of_two(1));
  REQUIRE(wh::core::is_power_of_two(2));
  REQUIRE_FALSE(wh::core::is_power_of_two(0));
  REQUIRE_FALSE(wh::core::is_power_of_two(3));

  REQUIRE(wh::core::align_up(10, 8) == 16);
  REQUIRE(wh::core::align_up(16, 8) == 16);
  REQUIRE(wh::core::align_up(9, 3) == 9);
}

TEST_CASE("compiler branch prediction helpers keep boolean semantics",
          "[core][compiler][branch]") {
  bool likely_taken = false;
  if (true)
    wh_likely { likely_taken = true; }
  REQUIRE(likely_taken);

  bool unlikely_taken = false;
  if (true)
    wh_unlikely { unlikely_taken = true; }
  REQUIRE(unlikely_taken);
}

TEST_CASE("compiler next_power_of_two handles edge values",
          "[core][compiler][extreme]") {
  REQUIRE(wh::core::next_power_of_two(0) == 1);
  REQUIRE(wh::core::next_power_of_two(1) == 1);
  REQUIRE(wh::core::next_power_of_two(1025) == 2048);

  constexpr std::size_t max_shiftable = static_cast<std::size_t>(1)
                                        << ((sizeof(std::size_t) * 8U) - 2U);
  REQUIRE(wh::core::next_power_of_two(max_shiftable) == max_shiftable);
}
