#include <string>

#include <catch2/catch_test_macros.hpp>

#include "wh/core/json/api.hpp"

TEST_CASE("json api parses serializes and queries members safely",
          "[UT][wh/core/json/api.hpp][parse_json][branch][boundary]") {
  const auto parsed = wh::core::parse_json(R"({"items":[1,2],"name":"alpha"})");
  REQUIRE(parsed.has_value());
  REQUIRE(parsed.value().IsObject());

  const auto items = wh::core::json_find_member(parsed.value(), "items");
  REQUIRE(items.has_value());
  REQUIRE(items.value()->IsArray());

  const auto second = wh::core::json_at(*items.value(), 1U);
  REQUIRE(second.has_value());
  REQUIRE(second.value()->GetInt() == 2);

  const auto missing = wh::core::json_find_member(parsed.value(), "missing");
  REQUIRE(missing.has_error());
  REQUIRE(missing.error() == wh::core::errc::not_found);

  const auto wrong_type = wh::core::json_at(parsed.value(), 0U);
  REQUIRE(wrong_type.has_error());
  REQUIRE(wrong_type.error() == wh::core::errc::type_mismatch);

  const auto serialized = wh::core::json_to_string(parsed.value());
  REQUIRE(serialized.has_value());
  REQUIRE(serialized.value().find("\"alpha\"") != std::string::npos);

  const auto failed = wh::core::parse_json("{]");
  REQUIRE(failed.has_error());
  REQUIRE(failed.error() == wh::core::errc::parse_error);

  const auto detailed = wh::core::parse_json_with_error("{]");
  REQUIRE(detailed.has_error());
  REQUIRE_FALSE(wh::core::parse_error_message(detailed.error()).empty());
}

TEST_CASE("json api preserves success details and reports lookup boundary failures",
          "[UT][wh/core/json/api.hpp][parse_json_with_error][condition][boundary]") {
  const auto parsed = wh::core::parse_json_with_error(R"({"name":"alpha"})");
  REQUIRE(parsed.has_value());
  REQUIRE(parsed.value().IsObject());

  const auto name = wh::core::json_find_member(parsed.value(), "name");
  REQUIRE(name.has_value());
  REQUIRE(name.value()->IsString());

  const auto array_lookup = wh::core::json_at(parsed.value(), 0U);
  REQUIRE(array_lookup.has_error());
  REQUIRE(array_lookup.error() == wh::core::errc::type_mismatch);
}
