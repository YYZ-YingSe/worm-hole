#include <catch2/catch_test_macros.hpp>

#include "wh/core/json/types.hpp"

static_assert(std::same_as<wh::core::json_size_type, rapidjson::SizeType>);

TEST_CASE("json types expose rapidjson aliases and parse error payload",
          "[UT][wh/core/json/types.hpp][json_parse_error][condition][branch][boundary]") {
  wh::core::json_document document{};
  document.SetObject();
  auto &allocator = document.GetAllocator();

  wh::core::json_value value{"text", allocator};
  const wh::core::json_parse_error error{rapidjson::kParseErrorValueInvalid, 7U};

  REQUIRE(value.IsString());
  REQUIRE(error.code == rapidjson::kParseErrorValueInvalid);
  REQUIRE(error.offset == 7U);
}

TEST_CASE("json type aliases expose document value allocator and stable kind defaults",
          "[UT][wh/core/json/types.hpp][json_value_kind][condition][boundary]") {
  STATIC_REQUIRE(std::same_as<wh::core::json_document, rapidjson::Document>);
  STATIC_REQUIRE(std::same_as<wh::core::json_value, rapidjson::Value>);
  STATIC_REQUIRE(std::same_as<wh::core::json_allocator, rapidjson::Document::AllocatorType>);

  const wh::core::json_value_kind null_kind = wh::core::json_value_kind::null_value;
  const wh::core::json_value_kind object_kind = wh::core::json_value_kind::object_value;
  REQUIRE(null_kind != object_kind);
}
