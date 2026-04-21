#include <catch2/catch_test_macros.hpp>

#include "wh/schema/tool.hpp"

TEST_CASE("tool facade builds parameter and default tool json schema",
          "[UT][wh/schema/tool.hpp][build_default_tool_json_schema][branch][boundary]") {
  wh::schema::tool_parameter_schema id{};
  id.name = "id";
  id.type = wh::schema::tool_parameter_type::integer;
  id.required = true;

  wh::schema::tool_parameter_schema tags{};
  tags.name = "tags";
  tags.type = wh::schema::tool_parameter_type::array;
  tags.item_types.push_back(wh::schema::tool_parameter_schema{
      .name = "tag", .type = wh::schema::tool_parameter_type::string});

  wh::schema::tool_schema_definition tool{};
  tool.name = "lookup";
  tool.description = "lookup by id";
  tool.parameters = {id, tags};

  auto schema = wh::schema::build_default_tool_json_schema(tool);
  REQUIRE(schema.has_value());
  auto function_member = wh::core::json_find_member(schema.value(), "function");
  REQUIRE(function_member.has_value());
  auto name_member = wh::core::json_find_member(*function_member.value(), "name");
  REQUIRE(name_member.has_value());
  REQUIRE(std::string{name_member.value()->GetString(), name_member.value()->GetStringLength()} ==
          "lookup");

  wh::schema::tool_schema_definition invalid{};
  auto invalid_schema = wh::schema::build_default_tool_json_schema(invalid);
  REQUIRE(invalid_schema.has_error());
  REQUIRE(invalid_schema.error() == wh::core::errc::invalid_argument);
}

TEST_CASE("tool facade reexports parameter type naming and default tool choice",
          "[UT][wh/schema/tool.hpp][detail::parameter_type_name][condition][branch]") {
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
