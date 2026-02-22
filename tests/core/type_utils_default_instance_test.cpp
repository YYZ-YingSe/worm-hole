#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <vector>

#include "wh/core/type_utils.hpp"

TEST_CASE("type_utils optional_result_sender traits",
          "[core][type_utils][condition]") {
  static_assert(wh::core::is_optional_v<std::optional<int>>);
  static_assert(!wh::core::is_optional_v<int>);
  static_assert(!wh::core::is_result_v<int>);
  static_assert(!wh::core::is_sender_v<int>);

  SUCCEED();
}

TEST_CASE("type_utils default_instance builds writable pointer chains",
          "[core][type_utils][branch]") {
  auto *first_level = wh::core::default_instance<int *>();
  REQUIRE(first_level != nullptr);
  *first_level = 7;
  REQUIRE(*first_level == 7);
  delete first_level;

  auto *second_level = wh::core::default_instance<int **>();
  REQUIRE(second_level != nullptr);
  REQUIRE(*second_level != nullptr);
  **second_level = 11;
  REQUIRE(**second_level == 11);
  delete *second_level;
  delete second_level;
}

TEST_CASE("type_utils reverse_copy and map_copy_as handle edge collections",
          "[core][type_utils][extreme]") {
  const std::vector<int> empty_values;
  const auto reversed_empty = wh::core::reverse_copy(empty_values);
  REQUIRE(reversed_empty.empty());

  const std::vector<int> numbers{1, 2, 3, 4};
  const auto reversed_numbers = wh::core::reverse_copy(numbers);
  REQUIRE(reversed_numbers == std::vector<int>({4, 3, 2, 1}));

  const std::map<int, int> source{{1, 2}, {3, 4}};
  const auto copied = wh::core::map_copy_as<std::map<int, int>>(source);
  REQUIRE(copied == source);
}
