#include <catch2/catch_test_macros.hpp>

#include "wh/compose/reduce/values_merge.hpp"

TEST_CASE("compose values merge delegates typed and dynamic merge paths",
          "[UT][wh/compose/reduce/values_merge.hpp][values_merge][condition][branch][boundary]") {
  wh::internal::values_merge_registry registry{};
  registry.reserve(4U);
  REQUIRE(registry.size() == 0U);
  REQUIRE_FALSE(registry.is_frozen());
  REQUIRE(registry
              .register_merge<int>([](std::span<const int> values)
                                       -> wh::core::result<int> {
                int sum = 0;
                for (const auto value : values) {
                  sum += value;
                }
                return sum;
              })
              .has_value());

  const std::array typed_values = {1, 2, 3};
  auto typed = wh::compose::values_merge(registry, std::span<const int>{typed_values});
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == 6);

  const std::array dynamic_values = {wh::core::any{1}, wh::core::any{5}};
  auto dynamic = wh::compose::values_merge(
      registry, wh::core::any_type_key_v<int>, dynamic_values);
  REQUIRE(dynamic.has_value());
  REQUIRE(*wh::core::any_cast<int>(&dynamic.value()) == 6);

  registry.freeze();
  REQUIRE(registry.is_frozen());
}

TEST_CASE("compose values merge surfaces empty singleton and unsupported branches",
          "[UT][wh/compose/reduce/values_merge.hpp][values_merge][condition][branch][boundary]") {
  wh::internal::values_merge_registry registry{};

  auto empty = wh::compose::values_merge(registry, std::span<const int>{});
  REQUIRE(empty.has_error());
  REQUIRE(empty.error() == wh::core::errc::invalid_argument);

  const std::array singleton = {7};
  auto single = wh::compose::values_merge(registry, std::span<const int>{singleton});
  REQUIRE(single.has_value());
  REQUIRE(single.value() == 7);

  const std::array dynamic_values = {wh::core::any{1}};
  auto unsupported = wh::compose::values_merge(
      registry, wh::core::any_type_key_v<int>, dynamic_values);
  REQUIRE(unsupported.has_error());
  REQUIRE(unsupported.error() == wh::core::errc::not_supported);
}
