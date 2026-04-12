#include <catch2/catch_test_macros.hpp>

#include "wh/core/small_vector.hpp"

TEST_CASE("small_vector facade exports vector concepts and metadata helpers",
          "[UT][wh/core/small_vector.hpp][small_vector_like][condition][branch][boundary]") {
  wh::core::default_small_vector<int, 4> values{};
  values.push_back(1);
  values.push_back(2);

  REQUIRE(values.size() == 2U);
  REQUIRE(wh::core::small_vector_like<decltype(values)>);
  REQUIRE(wh::core::describe_capacity(values).size == 2U);
}

TEST_CASE("small_vector facade exposes default alias growth and inline capacity behavior",
          "[UT][wh/core/small_vector.hpp][default_small_vector][condition][boundary]") {
  wh::core::default_small_vector<int, 2> values{};
  values.push_back(1);
  values.push_back(2);
  values.push_back(3);

  REQUIRE(values.size() == 3U);
  REQUIRE(values[0] == 1);
  REQUIRE(values[2] == 3);
  REQUIRE(wh::core::describe_capacity(values).capacity >= 3U);
}
