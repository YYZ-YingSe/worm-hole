#include <span>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "wh/internal/reduce_registry.hpp"

TEST_CASE("reduce registry registers typed reducers and bridges dynamic reduction",
          "[UT][wh/internal/"
          "reduce_registry.hpp][reduce_registry_core::register_reducer][branch][boundary]") {
  wh::internal::reduce_registry_core registry{};

  auto registered = registry.register_reducer<int>(
      [](const std::span<const int> values) -> wh::core::result<int> {
        int sum = 0;
        for (const auto value : values) {
          sum += value;
        }
        return sum;
      });
  REQUIRE(registered.has_value());
  REQUIRE(registry.size() == 1U);
  REQUIRE(registry.find_dynamic(wh::core::any_type_key_v<int>) != nullptr);
  REQUIRE(registry.find_typed<int>() != nullptr);

  const std::array values = {wh::core::any{1}, wh::core::any{2}, wh::core::any{3}};
  auto reduced = registry.reduce(wh::core::any_type_key_v<int>, values);
  REQUIRE(reduced.has_value());
  REQUIRE(*wh::core::any_cast<int>(&reduced.value()) == 6);

  auto duplicate = registry.register_reducer<int>(
      [](std::span<const int>) -> wh::core::result<int> { return 0; });
  REQUIRE(duplicate.has_error());
  REQUIRE(duplicate.error() == wh::core::errc::already_exists);

  const std::array mixed = {wh::core::any{1}, wh::core::any{std::string{"x"}}};
  auto mismatch = registry.reduce(wh::core::any_type_key_v<int>, mixed);
  REQUIRE(mismatch.has_error());
  REQUIRE(mismatch.error() == wh::core::errc::type_mismatch);
}

TEST_CASE("reduce registry pointer reducers freeze and reject invalid registrations",
          "[UT][wh/internal/"
          "reduce_registry.hpp][reduce_registry_core::register_reducer_from_ptrs][branch]") {
  wh::internal::reduce_registry_core registry{};

  auto invalid = registry.register_reducer<int>(nullptr);
  REQUIRE(invalid.has_error());
  REQUIRE(invalid.error() == wh::core::errc::invalid_argument);

  auto pointer_registered = registry.register_reducer_from_ptrs<std::string>(
      [](const std::span<const std::string *> values) -> wh::core::result<std::string> {
        std::string joined{};
        for (const auto *value : values) {
          joined += *value;
        }
        return joined;
      });
  REQUIRE(pointer_registered.has_value());

  registry.freeze();
  auto frozen = registry.register_reducer<double>(
      [](std::span<const double>) -> wh::core::result<double> { return 1.0; });
  REQUIRE(frozen.has_error());
  REQUIRE(frozen.error() == wh::core::errc::contract_violation);
}

TEST_CASE("reduce registry reports empty and unsupported dynamic reduction requests",
          "[UT][wh/internal/"
          "reduce_registry.hpp][reduce_registry_core::reduce][condition][branch][boundary]") {
  wh::internal::reduce_registry_core registry{};

  auto empty = registry.reduce(wh::core::any_type_key_v<int>,
                               std::span<const wh::internal::dynamic_reduce_value>{});
  REQUIRE(empty.has_error());
  REQUIRE(empty.error() == wh::core::errc::invalid_argument);

  const std::array unsupported = {wh::core::any{1}, wh::core::any{2}};
  auto no_handler = registry.reduce(wh::core::any_type_key_v<int>, unsupported);
  REQUIRE(no_handler.has_error());
  REQUIRE(no_handler.error() == wh::core::errc::not_supported);
}
