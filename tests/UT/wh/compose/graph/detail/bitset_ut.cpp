#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph/detail/bitset.hpp"

TEST_CASE("dynamic bitset resets tests sets clears and swaps runtime-sized words",
          "[UT][wh/compose/graph/detail/"
          "bitset.hpp][dynamic_bitset::set_if_unset][condition][branch][boundary]") {
  wh::compose::detail::dynamic_bitset bits{};
  REQUIRE(bits.size() == 0U);

  bits.reset(5U, false);
  REQUIRE(bits.size() == 5U);
  for (std::size_t index = 0U; index < bits.size(); ++index) {
    REQUIRE_FALSE(bits.test(index));
  }

  bits.set(1U);
  bits.set(4U);
  REQUIRE(bits.test(1U));
  REQUIRE(bits.test(4U));
  bits.clear(1U);
  REQUIRE_FALSE(bits.test(1U));
  REQUIRE_FALSE(bits.set_if_unset(4U));
  REQUIRE(bits.set_if_unset(3U));
  REQUIRE(bits.test(3U));

  wh::compose::detail::dynamic_bitset filled{70U, true};
  REQUIRE(filled.size() == 70U);
  REQUIRE(filled.test(0U));
  REQUIRE(filled.test(69U));

  bits.swap(filled);
  REQUIRE(bits.size() == 70U);
  REQUIRE(bits.test(69U));
  REQUIRE(filled.size() == 5U);
  REQUIRE(filled.test(4U));
}

TEST_CASE("dynamic bitset covers zero-sized resets and cross-word bit boundaries",
          "[UT][wh/compose/graph/detail/"
          "bitset.hpp][dynamic_bitset::reset][condition][branch][boundary]") {
  wh::compose::detail::dynamic_bitset bits{65U, false};
  REQUIRE(bits.size() == 65U);
  REQUIRE_FALSE(bits.test(63U));
  REQUIRE_FALSE(bits.test(64U));

  REQUIRE(bits.set_if_unset(63U));
  REQUIRE(bits.set_if_unset(64U));
  REQUIRE_FALSE(bits.set_if_unset(64U));
  REQUIRE(bits.test(63U));
  REQUIRE(bits.test(64U));

  bits.clear(64U);
  REQUIRE_FALSE(bits.test(64U));
  REQUIRE(bits.set_if_unset(64U));

  bits.reset(1U, true);
  REQUIRE(bits.size() == 1U);
  REQUIRE(bits.test(0U));

  bits.reset(0U, true);
  REQUIRE(bits.size() == 0U);

  wh::compose::detail::dynamic_bitset other{2U, true};
  bits.swap(other);
  REQUIRE(bits.size() == 2U);
  REQUIRE(bits.test(0U));
  REQUIRE(bits.test(1U));
  REQUIRE(other.size() == 0U);
}
