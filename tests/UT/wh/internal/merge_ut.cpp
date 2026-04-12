#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <unordered_map>

#include "wh/internal/merge.hpp"

static_assert(
    wh::internal::string_keyed_map_like<std::unordered_map<std::string, int>>);

TEST_CASE("values merge registry merges registered reducers and typed singletons",
          "[UT][wh/internal/merge.hpp][values_merge_registry::merge_as][branch][boundary]") {
  wh::internal::values_merge_registry registry{};

  auto registered = registry.register_merge<int>(
      [](const std::span<const int> values) -> wh::core::result<int> {
        int sum = 0;
        for (const auto value : values) {
          sum += value;
        }
        return sum;
      });
  REQUIRE(registered.has_value());

  const std::array typed_values = {1, 2, 3};
  auto typed = registry.merge_as<int>(typed_values);
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == 6);

  const std::array dynamic_values = {wh::core::any{1}, wh::core::any{5}};
  auto dynamic = registry.merge(wh::core::any_type_key_v<int>, dynamic_values);
  REQUIRE(dynamic.has_value());
  REQUIRE(*wh::core::any_cast<int>(&dynamic.value()) == 6);
}

TEST_CASE("values merge registry reports empty unsupported and singleton fallback paths",
          "[UT][wh/internal/merge.hpp][values_merge_registry::merge][branch]") {
  wh::internal::values_merge_registry registry{};

  auto empty = registry.merge_as<int>(std::span<const int>{});
  REQUIRE(empty.has_error());
  REQUIRE(empty.error() == wh::core::errc::invalid_argument);

  const std::array singleton = {7};
  auto single = registry.merge_as<int>(singleton);
  REQUIRE(single.has_value());
  REQUIRE(single.value() == 7);

  const std::array unsupported_values = {wh::core::any{1}};
  auto unsupported =
      registry.merge(wh::core::any_type_key_v<int>, unsupported_values);
  REQUIRE(unsupported.has_error());
  REQUIRE(unsupported.error() == wh::core::errc::not_supported);
}

TEST_CASE(
    "values merge registry supports pointer reducers duplicate guards and freeze",
    "[UT][wh/internal/merge.hpp][values_merge_registry::register_merge_from_ptrs][condition][branch][boundary]") {
  wh::internal::values_merge_registry registry{};
  registry.reserve(2U);

  auto pointer_registered = registry.register_merge_from_ptrs<std::string>(
      [](const std::span<const std::string *> values)
          -> wh::core::result<std::string> {
        std::string joined{};
        for (const auto *value : values) {
          joined += *value;
        }
        return joined;
      });
  REQUIRE(pointer_registered.has_value());
  REQUIRE(registry.size() == 1U);
  REQUIRE(registry.find_merge(wh::core::any_type_key_v<std::string>) != nullptr);

  const std::array strings = {std::string{"x"}, std::string{"y"}};
  auto merged_strings = registry.merge_as<std::string>(strings);
  REQUIRE(merged_strings.has_value());
  REQUIRE(merged_strings.value() == "xy");

  auto duplicate = registry.register_merge_from_ptrs<std::string>(
      [](std::span<const std::string *>) -> wh::core::result<std::string> {
        return std::string{"duplicate"};
      });
  REQUIRE(duplicate.has_error());
  REQUIRE(duplicate.error() == wh::core::errc::already_exists);

  registry.freeze();
  auto frozen = registry.register_merge<double>(
      [](std::span<const double>) -> wh::core::result<double> { return 1.0; });
  REQUIRE(frozen.has_error());
  REQUIRE(frozen.error() == wh::core::errc::contract_violation);
}
