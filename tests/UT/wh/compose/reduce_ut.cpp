#include <array>
#include <span>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "wh/compose/reduce.hpp"

TEST_CASE("compose reduce facade exposes values merge helpers",
          "[UT][wh/compose/reduce.hpp][values_merge][condition][branch][boundary]") {
  wh::internal::values_merge_registry merge_registry{};
  REQUIRE(merge_registry
              .register_merge<int>([](std::span<const int> values) -> wh::core::result<int> {
                int sum = 0;
                for (const auto value : values) {
                  sum += value;
                }
                return sum;
              })
              .has_value());
  const std::array values = {1, 2};
  auto merged = wh::compose::values_merge(merge_registry, std::span<const int>{values});

  REQUIRE(merged.has_value());
  REQUIRE(merged.value() == 3);
}

TEST_CASE("compose reduce facade exposes stream concat helpers",
          "[UT][wh/compose/reduce.hpp][stream_concat][condition][branch][boundary]") {
  wh::internal::stream_concat_registry registry{};
  REQUIRE(registry
              .register_concat<std::string>(
                  [](std::span<const std::string> values) -> wh::core::result<std::string> {
                    std::string joined{};
                    for (const auto &value : values) {
                      joined += value;
                    }
                    return joined;
                  })
              .has_value());

  const std::array values = {std::string{"a"}, std::string{"b"}};
  auto merged = wh::compose::stream_concat(registry, std::span<const std::string>{values});

  REQUIRE(merged.has_value());
  REQUIRE(merged.value() == "ab");
}
