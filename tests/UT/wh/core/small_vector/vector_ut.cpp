#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "wh/core/small_vector/vector.hpp"

TEST_CASE("small_vector vector supports inline growth erase and std round-trip",
          "[UT][wh/core/small_vector/vector.hpp][small_vector][branch][boundary]") {
  wh::core::small_vector<int, 4> values{};
  REQUIRE(values.empty());
  REQUIRE(values.using_inline_storage());
  REQUIRE(values.is_small());

  values.push_back(1);
  values.push_back(2);
  values.push_back(3);
  const auto inserted = values.insert(values.begin() + 1, 9);
  REQUIRE(values.size() == 4U);
  REQUIRE(inserted == values.begin() + 1);
  REQUIRE(values[1] == 9);

  values.push_back(5);
  REQUIRE(values.size() == 5U);
  REQUIRE_FALSE(values.using_inline_storage());

  values.erase(values.begin() + 1);
  REQUIRE(values.size() == 4U);
  REQUIRE(values[1] == 2);

  const auto removed_equal = wh::core::erase(values, 2);
  REQUIRE(removed_equal == 1U);
  const auto removed_if = wh::core::erase_if(values, [](int value) {
    return (value % 2) != 0;
  });
  REQUIRE(removed_if == 3U);
  REQUIRE(values.empty());

  const std::vector<int> standard{4, 5, 6};
  auto from_std = wh::core::small_vector<int, 4>::from_std_vector(standard);
  REQUIRE(from_std.to_std_vector() == standard);

  from_std.resize(2U);
  from_std.shrink_to_fit();
  REQUIRE(from_std.using_inline_storage());
}

TEST_CASE("small_vector default_init and base interface stay usable",
          "[UT][wh/core/small_vector/vector.hpp][default_init][branch]") {
  wh::core::small_vector<std::string, 2> values(2U, wh::core::default_init);
  wh::core::small_vector_base<std::string> &base = values;

  REQUIRE(values.size() == 2U);
  REQUIRE(base.size() == 2U);
  REQUIRE(base.capacity() >= 2U);
  REQUIRE(base.data() == values.data());
}

TEST_CASE("small_vector vector handles empty round-trip range erase and swap boundaries",
          "[UT][wh/core/small_vector/vector.hpp][erase][condition][boundary]") {
  auto empty = wh::core::small_vector<int, 2>::from_std_vector({});
  REQUIRE(empty.empty());
  REQUIRE(empty.using_inline_storage());
  REQUIRE(empty.to_std_vector().empty());
  REQUIRE(wh::core::erase(empty, 7) == 0U);
  REQUIRE(wh::core::erase_if(empty, [](int) { return true; }) == 0U);

  wh::core::small_vector<int, 2> values{};
  values.push_back(1);
  values.push_back(2);
  values.push_back(3);
  values.push_back(4);
  values.erase(values.begin() + 1, values.begin() + 3);
  REQUIRE(values.to_std_vector() == std::vector<int>({1, 4}));

  wh::core::small_vector<int, 2> other{};
  other.push_back(9);
  swap(values, other);
  REQUIRE(values.to_std_vector() == std::vector<int>({9}));
  REQUIRE(other.to_std_vector() == std::vector<int>({1, 4}));
}
