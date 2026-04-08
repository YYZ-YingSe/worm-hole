#include <catch2/catch_test_macros.hpp>

#include <string>
#include <type_traits>

#include "wh/core/json.hpp"

namespace {

static_assert(wh::core::json_value_like<wh::core::json_value>);
static_assert(wh::core::json_value_like<wh::core::json_document>);

} // namespace

TEST_CASE("json kinds and aliases expose rapidjson surface",
          "[UT][wh/core/json.hpp][json_kind][branch]") {
  wh::core::json_document document{};
  document.SetObject();
  auto &allocator = document.GetAllocator();

  wh::core::json_value null_value{};
  wh::core::json_value bool_value{true};
  wh::core::json_value number_value{7};
  wh::core::json_value string_value{"text", allocator};
  wh::core::json_value array_value{rapidjson::kArrayType};
  wh::core::json_value object_value{rapidjson::kObjectType};

  REQUIRE(wh::core::json_kind(null_value) == wh::core::json_value_kind::null_value);
  REQUIRE(wh::core::json_kind(bool_value) == wh::core::json_value_kind::bool_value);
  REQUIRE(wh::core::json_kind(number_value) ==
          wh::core::json_value_kind::number_value);
  REQUIRE(wh::core::json_kind(string_value) ==
          wh::core::json_value_kind::string_value);
  REQUIRE(wh::core::json_kind(array_value) ==
          wh::core::json_value_kind::array_value);
  REQUIRE(wh::core::json_kind(object_value) ==
          wh::core::json_value_kind::object_value);
}

TEST_CASE("json parse helpers expose success and detailed failure information",
          "[UT][wh/core/json.hpp][parse_json][branch][boundary]") {
  const auto parsed = wh::core::parse_json(R"({"items":[1,2],"name":"alpha"})");
  REQUIRE(parsed.has_value());
  REQUIRE(parsed.value().IsObject());

  const auto fast_fail = wh::core::parse_json("{]");
  REQUIRE(fast_fail.has_error());
  REQUIRE(fast_fail.error() == wh::core::errc::parse_error);

  const auto detailed_fail = wh::core::parse_json_with_error("{]");
  REQUIRE(detailed_fail.has_error());
  REQUIRE(detailed_fail.error().code != rapidjson::kParseErrorNone);
  REQUIRE_FALSE(wh::core::parse_error_message(detailed_fail.error()).empty());
}

TEST_CASE("json member lookup index lookup and stringify preserve structure",
          "[UT][wh/core/json.hpp][json_find_member][branch][boundary]") {
  const auto parsed = wh::core::parse_json(R"({"items":[1,2],"name":"alpha"})");
  REQUIRE(parsed.has_value());

  const auto items_member = wh::core::json_find_member(parsed.value(), "items");
  REQUIRE(items_member.has_value());
  REQUIRE(items_member.value()->IsArray());

  const auto second_item = wh::core::json_at(*items_member.value(), 1U);
  REQUIRE(second_item.has_value());
  REQUIRE(second_item.value()->GetInt() == 2);

  const auto missing_member = wh::core::json_find_member(parsed.value(), "missing");
  REQUIRE(missing_member.has_error());
  REQUIRE(missing_member.error() == wh::core::errc::not_found);

  const auto wrong_type = wh::core::json_at(parsed.value(), 0U);
  REQUIRE(wrong_type.has_error());
  REQUIRE(wrong_type.error() == wh::core::errc::type_mismatch);

  const auto out_of_range = wh::core::json_at(*items_member.value(), 5U);
  REQUIRE(out_of_range.has_error());
  REQUIRE(out_of_range.error() == wh::core::errc::not_found);

  const auto serialized = wh::core::json_to_string(parsed.value());
  REQUIRE(serialized.has_value());
  REQUIRE(serialized.value().find("\"items\"") != std::string::npos);
  REQUIRE(serialized.value().find("\"alpha\"") != std::string::npos);
}

TEST_CASE("json facade reports object-versus-array mismatches and parse offsets",
          "[UT][wh/core/json.hpp][parse_json_with_error][condition][branch][boundary]") {
  const auto parsed_array = wh::core::parse_json(R"([1,2,3])");
  REQUIRE(parsed_array.has_value());

  const auto member_on_array = wh::core::json_find_member(parsed_array.value(), "x");
  REQUIRE(member_on_array.has_error());
  REQUIRE(member_on_array.error() == wh::core::errc::type_mismatch);

  const auto detailed = wh::core::parse_json_with_error("{");
  REQUIRE(detailed.has_error());
  REQUIRE(detailed.error().offset >= 1U);
  REQUIRE_FALSE(wh::core::parse_error_message(detailed.error()).empty());
}
