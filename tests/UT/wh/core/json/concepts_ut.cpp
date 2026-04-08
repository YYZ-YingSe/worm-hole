#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "wh/core/json/concepts.hpp"

static_assert(wh::core::json_value_like<rapidjson::Value>);
static_assert(wh::core::json_value_like<rapidjson::Document>);
static_assert(!wh::core::json_value_like<int>);
static_assert(!wh::core::json_value_like<std::vector<int>>);

TEST_CASE("json concepts accept rapidjson value-like types",
          "[UT][wh/core/json/concepts.hpp][json_value_like][condition][branch][boundary]") {
  STATIC_REQUIRE(wh::core::json_value_like<rapidjson::Value>);
  STATIC_REQUIRE(wh::core::json_value_like<rapidjson::Document>);
  STATIC_REQUIRE_FALSE(wh::core::json_value_like<int>);

  rapidjson::Value value{};
  value.SetArray();
  REQUIRE(value.IsArray());
}

TEST_CASE("json concepts also admit rapidjson document mutation surface",
          "[UT][wh/core/json/concepts.hpp][json_value_like][condition][boundary]") {
  rapidjson::Document document{};
  document.SetObject();
  REQUIRE(document.IsObject());
  REQUIRE(document.MemberCount() == 0U);

  document.SetArray();
  REQUIRE(document.IsArray());
  REQUIRE(document.Size() == 0U);
}
