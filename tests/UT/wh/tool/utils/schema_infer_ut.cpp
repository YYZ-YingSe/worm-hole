#include <catch2/catch_test_macros.hpp>

#include "wh/tool/utils/schema_infer.hpp"

TEST_CASE("infer_schema builds base definitions with and without parameters",
          "[UT][wh/tool/utils/schema_infer.hpp][infer_schema][branch][boundary]") {
  auto empty = wh::tool::utils::infer_schema<int>("lookup", "desc");
  REQUIRE(empty.name == "lookup");
  REQUIRE(empty.description == "desc");
  REQUIRE(empty.parameters.empty());

  wh::schema::tool_parameter_schema child{};
  child.name = "limit";
  child.type = wh::schema::tool_parameter_type::integer;

  wh::schema::tool_parameter_schema root{};
  root.name = "query";
  root.type = wh::schema::tool_parameter_type::object;
  root.properties.push_back(child);

  auto modified = wh::tool::utils::infer_schema(
      "lookup", "desc", std::vector<wh::schema::tool_parameter_schema>{root},
      [](wh::schema::tool_parameter_schema &schema, const std::string_view path) {
        schema.description = std::string{path};
      });

  REQUIRE(modified.parameters.size() == 1U);
  REQUIRE(modified.parameters.front().description == "query");
  REQUIRE(modified.parameters.front().properties.front().description ==
          "query.limit");
}

TEST_CASE("infer_schema keeps supplied parameter descriptions when no mutator is provided",
          "[UT][wh/tool/utils/schema_infer.hpp][infer_schema][condition][boundary]") {
  wh::schema::tool_parameter_schema parameter{};
  parameter.name = "query";
  parameter.type = wh::schema::tool_parameter_type::string;
  parameter.description = "pre-filled";

  auto inferred = wh::tool::utils::infer_schema(
      "lookup", "desc", std::vector<wh::schema::tool_parameter_schema>{parameter});
  REQUIRE(inferred.name == "lookup");
  REQUIRE(inferred.parameters.size() == 1U);
  REQUIRE(inferred.parameters.front().description == "pre-filled");
}
