#include <catch2/catch_test_macros.hpp>

#include "wh/compose/field/mapping.hpp"

TEST_CASE("field mapping parser accepts valid paths and rejects malformed ones",
          "[UT][wh/compose/field/mapping.hpp][parse_field_path][condition][branch][boundary]") {
  auto parsed = wh::compose::parse_field_path("input.user.id");
  REQUIRE(parsed.has_value());
  REQUIRE(parsed.value().segments == std::vector<std::string>{"input", "user", "id"});

  auto empty = wh::compose::parse_field_path("");
  REQUIRE(empty.has_error());
  REQUIRE(empty.error() == wh::core::errc::invalid_argument);

  auto malformed = wh::compose::parse_field_path("input..id");
  REQUIRE(malformed.has_error());
  REQUIRE(malformed.error() == wh::core::errc::invalid_argument);
}

TEST_CASE(
    "field mapping rule compilation skips source path for static or extractor rules",
    "[UT][wh/compose/field/mapping.hpp][compile_field_mapping_rule][condition][branch][boundary]") {
  wh::compose::field_mapping_rule static_rule{};
  static_rule.to_path = "output.value";
  static_rule.static_value = wh::compose::graph_value{7};

  auto compiled_static = wh::compose::compile_field_mapping_rule(static_rule);
  REQUIRE(compiled_static.has_value());
  REQUIRE_FALSE(compiled_static.value().from_path.has_value());

  wh::compose::field_mapping_rule extractor_rule{};
  extractor_rule.to_path = "output.value";
  extractor_rule.extractor =
      [](const wh::compose::graph_value_map &,
         wh::core::run_context &) -> wh::core::result<wh::compose::graph_value> { return 9; };
  auto compiled_extractor = wh::compose::compile_field_mapping_rule(extractor_rule);
  REQUIRE(compiled_extractor.has_value());
  REQUIRE_FALSE(compiled_extractor.value().from_path.has_value());
}

TEST_CASE("field mapping parser rejects control characters and preserves original text on success",
          "[UT][wh/compose/field/mapping.hpp][parse_field_path][condition][branch][boundary]") {
  auto parsed = wh::compose::parse_field_path("request.user_id");
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->text == "request.user_id");

  const std::string malformed{"input.\x1Fvalue"};
  auto invalid = wh::compose::parse_field_path(malformed);
  REQUIRE(invalid.has_error());
  REQUIRE(invalid.error() == wh::core::errc::invalid_argument);
}

TEST_CASE(
    "field mapping compilation parses source paths for dynamic rules and rejects invalid "
    "destinations",
    "[UT][wh/compose/field/mapping.hpp][compile_field_mapping_rule][condition][branch][boundary]") {
  wh::compose::field_mapping_rule dynamic_rule{};
  dynamic_rule.from_path = "input.user.id";
  dynamic_rule.to_path = "output.user_id";
  auto compiled_dynamic = wh::compose::compile_field_mapping_rule(dynamic_rule);
  REQUIRE(compiled_dynamic.has_value());
  REQUIRE(compiled_dynamic->from_path.has_value());
  REQUIRE(compiled_dynamic->from_path->segments == std::vector<std::string>{"input", "user", "id"});

  wh::compose::field_mapping_rule invalid_rule{};
  invalid_rule.from_path = "input.user.id";
  invalid_rule.to_path = "";
  auto invalid = wh::compose::compile_field_mapping_rule(invalid_rule);
  REQUIRE(invalid.has_error());
  REQUIRE(invalid.error() == wh::core::errc::invalid_argument);
}
