#include <catch2/catch_test_macros.hpp>

#include <array>
#include <span>

#include "wh/internal/concat.hpp"

namespace {

struct adl_concat_value {
  int value{0};
};

[[nodiscard]] auto wh_stream_concat(const std::span<const adl_concat_value> values)
    -> wh::core::result<adl_concat_value> {
  int sum = 0;
  for (const auto value : values) {
    sum += value.value;
  }
  return adl_concat_value{sum};
}

} // namespace

TEST_CASE("stream concat registry uses adl customizations and registered bridges",
          "[UT][wh/internal/concat.hpp][stream_concat_registry::concat_as][branch][boundary]") {
  wh::internal::stream_concat_registry registry{};

  const std::array adl_values = {adl_concat_value{1}, adl_concat_value{2}};
  auto adl = registry.concat_as<adl_concat_value>(adl_values);
  REQUIRE(adl.has_value());
  REQUIRE(adl.value().value == 3);

  auto registered = registry.register_concat<int>(
      [](const std::span<const int> values) -> wh::core::result<int> {
        int sum = 0;
        for (const auto value : values) {
          sum += value;
        }
        return sum;
      });
  REQUIRE(registered.has_value());

  const std::array dynamic_values = {wh::core::any{1}, wh::core::any{4}};
  auto dynamic =
      registry.concat(wh::core::any_type_key_v<int>, dynamic_values);
  REQUIRE(dynamic.has_value());
  REQUIRE(*wh::core::any_cast<int>(&dynamic.value()) == 5);
}

TEST_CASE("stream concat registry reports invalid empty inputs and type mismatches",
          "[UT][wh/internal/concat.hpp][stream_concat_registry::concat][branch]") {
  wh::internal::stream_concat_registry registry{};

  auto empty = registry.concat_as<int>(std::span<const int>{});
  REQUIRE(empty.has_error());
  REQUIRE(empty.error() == wh::core::errc::invalid_argument);

  const std::array mismatch_values = {wh::core::any{std::string{"x"}}};
  auto mismatch =
      registry.concat(wh::core::any_type_key_v<int>, mismatch_values);
  REQUIRE(mismatch.has_error());
  REQUIRE(mismatch.error() == wh::core::errc::type_mismatch);
}

TEST_CASE(
    "stream concat registry honors reserve freeze pointer reducers and fallback branches",
    "[UT][wh/internal/concat.hpp][stream_concat_registry::register_concat_from_ptrs][condition][branch][boundary]") {
  wh::internal::stream_concat_registry registry{};
  registry.reserve(4U);
  REQUIRE_FALSE(registry.is_frozen());

  auto pointer_registered = registry.register_concat_from_ptrs<std::string>(
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
  REQUIRE(registry.find_concat(wh::core::any_type_key_v<std::string>) != nullptr);

  const std::array typed_values = {std::string{"a"}, std::string{"b"}};
  auto typed = registry.concat_as<std::string>(typed_values);
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == "ab");

  const std::array single = {wh::core::any{7}};
  auto single_value = registry.concat(wh::core::any_type_key_v<int>, single);
  REQUIRE(single_value.has_value());
  REQUIRE(*wh::core::any_cast<int>(&single_value.value()) == 7);

  const std::array unsupported_values = {wh::core::any{1}, wh::core::any{2}};
  auto unsupported =
      registry.concat(wh::core::any_type_key_v<int>, unsupported_values);
  REQUIRE(unsupported.has_error());
  REQUIRE(unsupported.error() == wh::core::errc::not_supported);

  registry.freeze();
  REQUIRE(registry.is_frozen());
  auto frozen = registry.register_concat<int>(
      [](std::span<const int>) -> wh::core::result<int> { return 0; });
  REQUIRE(frozen.has_error());
  REQUIRE(frozen.error() == wh::core::errc::contract_violation);
}
