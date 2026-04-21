#include <array>

#include <catch2/catch_test_macros.hpp>

#include "wh/schema/tool/types.hpp"

TEST_CASE("tool schema types build sorted parameter json schema with required fields",
          "[UT][wh/schema/tool/types.hpp][build_parameters_json_schema][branch][boundary]") {
  wh::schema::tool_parameter_schema tags{};
  tags.name = "tags";
  tags.type = wh::schema::tool_parameter_type::array;
  tags.item_types.push_back(wh::schema::tool_parameter_schema{
      .name = "tag", .type = wh::schema::tool_parameter_type::string});

  wh::schema::tool_parameter_schema id{};
  id.name = "id";
  id.type = wh::schema::tool_parameter_type::integer;
  id.required = true;

  auto schema = wh::schema::build_parameters_json_schema(std::array{id, tags});
  REQUIRE(schema.has_value());

  auto properties = wh::core::json_find_member(schema.value(), "properties");
  REQUIRE(properties.has_value());
  auto id_schema = wh::core::json_find_member(*properties.value(), "id");
  REQUIRE(id_schema.has_value());
  auto tags_schema = wh::core::json_find_member(*properties.value(), "tags");
  REQUIRE(tags_schema.has_value());
}

TEST_CASE("tool schema types build default function export and reject invalid raw schema",
          "[UT][wh/schema/tool/types.hpp][build_default_tool_json_schema][branch]") {
  wh::schema::tool_schema_definition definition{};
  definition.name = "search";
  definition.description = "lookup";
  definition.raw_parameters_json_schema = R"({"type":"object","properties":{}})";

  auto built = wh::schema::build_default_tool_json_schema(definition);
  REQUIRE(built.has_value());
  auto function_member = wh::core::json_find_member(built.value(), "function");
  REQUIRE(function_member.has_value());

  definition.raw_parameters_json_schema = "{";
  auto invalid = wh::schema::build_default_tool_json_schema(definition);
  REQUIRE(invalid.has_error());
  REQUIRE(invalid.error() == wh::core::errc::parse_error);
}

TEST_CASE("tool schema types expose parameter type names and default tool choice",
          "[UT][wh/schema/tool/types.hpp][detail::parameter_type_name][condition]") {
  REQUIRE(wh::schema::detail::parameter_type_name(wh::schema::tool_parameter_type::string) ==
          "string");
  REQUIRE(wh::schema::detail::parameter_type_name(wh::schema::tool_parameter_type::integer) ==
          "integer");
  REQUIRE(wh::schema::detail::parameter_type_name(wh::schema::tool_parameter_type::number) ==
          "number");
  REQUIRE(wh::schema::detail::parameter_type_name(wh::schema::tool_parameter_type::boolean) ==
          "boolean");
  REQUIRE(wh::schema::detail::parameter_type_name(wh::schema::tool_parameter_type::object) ==
          "object");
  REQUIRE(wh::schema::detail::parameter_type_name(wh::schema::tool_parameter_type::array) ==
          "array");

  wh::schema::tool_choice choice{};
  REQUIRE(choice.mode == wh::schema::tool_call_mode::allow);
}
