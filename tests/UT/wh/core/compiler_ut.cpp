#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "wh/core/compiler.hpp"

static_assert(wh::core::trivially_relocatable<int>);
static_assert(wh::core::trivially_copyable_value<int>);
static_assert(!std::is_same_v<decltype(wh::core::active_compiler), int>);

TEST_CASE("compiler helpers expose platform alignment and contract metadata",
          "[UT][wh/core/compiler.hpp][is_power_of_two][branch][boundary]") {
  REQUIRE(wh::core::compiler_version_major >= 0);
  REQUIRE(wh::core::default_cacheline_size > 0U);

  REQUIRE(wh::core::is_power_of_two(1U));
  REQUIRE(wh::core::is_power_of_two(2U));
  REQUIRE_FALSE(wh::core::is_power_of_two(0U));
  REQUIRE_FALSE(wh::core::is_power_of_two(3U));

  REQUIRE(wh::core::align_up(10U, 8U) == 16U);
  REQUIRE(wh::core::align_up(16U, 8U) == 16U);
  REQUIRE(wh::core::align_up(9U, 3U) == 9U);

  REQUIRE(wh::core::next_power_of_two(0U) == 1U);
  REQUIRE(wh::core::next_power_of_two(1U) == 1U);
  REQUIRE(wh::core::next_power_of_two(1025U) == 2048U);
}

TEST_CASE("compiler helpers preserve boolean branch and contract semantics",
          "[UT][wh/core/compiler.hpp][contract_kind_name][condition][branch]") {
  REQUIRE(wh::core::contract_kind_name(wh::core::contract_kind::precondition) == "precondition");
  REQUIRE(wh::core::contract_kind_name(wh::core::contract_kind::postcondition) == "postcondition");
  REQUIRE(wh::core::contract_kind_name(wh::core::contract_kind::invariant) == "invariant");

  bool likely_taken = false;
  if (true)
    wh_likely { likely_taken = true; }
  REQUIRE(likely_taken);

  bool unlikely_taken = false;
  if (true)
    wh_unlikely { unlikely_taken = true; }
  REQUIRE(unlikely_taken);

  wh::core::assume(true);
  wh::core::spin_pause();
  wh_precondition(true);
  wh_postcondition(true);
  wh_invariant(true);
}

TEST_CASE("compiler helpers expose compiler identity booleans consistently",
          "[UT][wh/core/compiler.hpp][active_compiler][condition][boundary]") {
  const auto active_flags = static_cast<int>(wh::core::compiler_is_clang) +
                            static_cast<int>(wh::core::compiler_is_gcc) +
                            static_cast<int>(wh::core::compiler_is_msvc);

  if (wh::core::active_compiler == wh::core::compiler_id::unknown) {
    REQUIRE(active_flags == 0);
  } else {
    REQUIRE(active_flags == 1);
  }

  REQUIRE(wh::core::align_up(0U, 8U) == 0U);
  REQUIRE(wh::core::align_up(17U, 16U) == 32U);
  REQUIRE(wh::core::next_power_of_two(2U) == 2U);
  REQUIRE(wh::core::next_power_of_two(3U) == 4U);
}
