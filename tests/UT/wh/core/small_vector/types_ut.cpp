#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "wh/core/small_vector/types.hpp"

TEST_CASE("small_vector metadata helpers describe capacity growth and contract",
          "[UT][wh/core/small_vector/types.hpp][describe_capacity][branch]") {
  wh::core::default_small_vector<int, 4> values{};
  values.push_back(1);
  values.push_back(2);

  const auto capacity = wh::core::describe_capacity(values);
  const auto growth = wh::core::describe_growth_policy(values);
  const auto contract = wh::core::describe_contract(values);

  REQUIRE(capacity.size == 2U);
  REQUIRE(capacity.capacity >= 4U);
  REQUIRE(capacity.inline_capacity == 4U);

  REQUIRE(growth.growth_multiplier_num == wh::core::small_vector_default_options::growth_numerator);
  REQUIRE(growth.growth_multiplier_den ==
          wh::core::small_vector_default_options::growth_denominator);
  REQUIRE(growth.heap_enabled == wh::core::small_vector_default_options::heap_enabled);

  REQUIRE(contract.small_object_optimized);
  REQUIRE(contract.preserves_insertion_order);
  REQUIRE(contract.supports_std_vector_round_trip);
  REQUIRE(contract.push_back_without_growth == wh::core::complexity_class::constant);
}

TEST_CASE("small_vector option types expose configured growth knobs",
          "[UT][wh/core/small_vector/types.hpp][small_vector_growth_policy][boundary]") {
  using custom_options = wh::core::small_vector_options<2U, 1U, 8U, false, false, std::uint16_t>;
  wh::core::small_vector<int, 2U, std::allocator<int>, custom_options> values{};

  const auto growth = wh::core::describe_growth_policy(values);
  REQUIRE(growth.minimum_dynamic_capacity == 8U);
  REQUIRE(growth.growth_multiplier_num == 2U);
  REQUIRE(growth.growth_multiplier_den == 1U);
  REQUIRE_FALSE(growth.heap_enabled);
  REQUIRE_FALSE(growth.shrink_to_inline);
}

TEST_CASE("small_vector metadata helpers also describe empty-vector boundaries",
          "[UT][wh/core/small_vector/types.hpp][describe_capacity][condition][boundary]") {
  wh::core::default_small_vector<int, 3> values{};

  const auto capacity = wh::core::describe_capacity(values);
  const auto contract = wh::core::describe_contract(values);

  REQUIRE(capacity.size == 0U);
  REQUIRE(capacity.capacity >= 3U);
  REQUIRE(capacity.inline_capacity == 3U);

  REQUIRE(contract.small_object_optimized);
  REQUIRE(contract.supports_custom_allocator);
  REQUIRE(contract.supports_custom_options);
  REQUIRE(contract.push_back_with_growth == wh::core::complexity_class::amortized_constant);
}
