#include <catch2/catch_test_macros.hpp>

#include "wh/compose/field/apply.hpp"

TEST_CASE("field apply maps nested values and static values into target maps",
          "[UT][wh/compose/field/apply.hpp][apply_field_mappings][condition][branch][boundary]") {
  wh::compose::graph_value_map source{};
  source.insert_or_assign(
      "input",
      wh::compose::graph_value{wh::compose::graph_value_map{
          {"user", wh::compose::graph_value{wh::compose::graph_value_map{
                        {"id", wh::compose::graph_value{7}},
                    }}}}});

  wh::compose::field_mapping_rule dynamic_rule{};
  dynamic_rule.from_path = "input.user.id";
  dynamic_rule.to_path = "request.user_id";

  wh::compose::field_mapping_rule static_rule{};
  static_rule.to_path = "request.source";
  static_rule.static_value = wh::compose::graph_value{std::string{"ut"}};

  wh::core::run_context context{};
  auto mapped = wh::compose::apply_field_mappings(
      source, wh::compose::graph_value_map{}, std::vector{dynamic_rule, static_rule},
      context);
  REQUIRE(mapped.has_value());

  auto request =
      wh::core::any_cast<wh::compose::graph_value_map>(&mapped.value().at("request"));
  REQUIRE(request != nullptr);
  REQUIRE(*wh::core::any_cast<int>(&request->at("user_id")) == 7);
  REQUIRE(*wh::core::any_cast<std::string>(&request->at("source")) == "ut");
}

TEST_CASE("field apply honors missing policy skip and surfaces failures otherwise",
          "[UT][wh/compose/field/apply.hpp][apply_field_mappings_in_place][condition][branch][boundary]") {
  wh::compose::graph_value_map target{};

  wh::compose::field_mapping_rule skip_rule{};
  skip_rule.from_path = "missing.value";
  skip_rule.to_path = "out.value";
  skip_rule.missing_policy = wh::compose::field_missing_policy::skip;

  wh::compose::field_mapping_rule fail_rule = skip_rule;
  fail_rule.missing_policy = wh::compose::field_missing_policy::fail;

  wh::core::run_context context{};
  auto skipped = wh::compose::apply_field_mappings(
      wh::compose::graph_value_map{}, target, std::vector{skip_rule}, context);
  REQUIRE(skipped.has_value());
  REQUIRE(skipped.value().empty());

  auto failed = wh::compose::apply_field_mappings(
      wh::compose::graph_value_map{}, target, std::vector{fail_rule}, context);
  REQUIRE(failed.has_error());
  REQUIRE(failed.error() == wh::core::errc::not_found);
}

TEST_CASE("field apply uses extractor callbacks and rejects rules without any readable source",
          "[UT][wh/compose/field/apply.hpp][apply_field_mappings][condition][branch][boundary]") {
  wh::compose::field_mapping_rule extractor_rule{};
  extractor_rule.to_path = "out.answer";
  extractor_rule.extractor =
      [](const wh::compose::graph_value_map &, wh::core::run_context &)
          -> wh::core::result<wh::compose::graph_value> { return 9; };

  wh::compose::field_mapping_rule invalid_rule{};
  invalid_rule.to_path = "out.invalid";

  wh::core::run_context context{};
  auto extracted = wh::compose::apply_field_mappings(
      wh::compose::graph_value_map{}, wh::compose::graph_value_map{},
      std::vector{extractor_rule}, context);
  REQUIRE(extracted.has_value());
  auto *out =
      wh::core::any_cast<wh::compose::graph_value_map>(&extracted->at("out"));
  REQUIRE(out != nullptr);
  REQUIRE(*wh::core::any_cast<int>(&out->at("answer")) == 9);

  auto invalid = wh::compose::apply_field_mappings(
      wh::compose::graph_value_map{}, wh::compose::graph_value_map{},
      std::vector{invalid_rule}, context);
  REQUIRE(invalid.has_error());
  REQUIRE(invalid.error() == wh::core::errc::invalid_argument);
}

TEST_CASE("field apply in-place staging does not leak earlier writes when a later rule fails",
          "[UT][wh/compose/field/apply.hpp][apply_field_mappings_in_place][condition][branch][boundary]") {
  wh::compose::graph_value_map target{};
  target.insert_or_assign("input", wh::compose::graph_value{7});

  wh::compose::compiled_field_mapping_rule first{};
  first.from_path = wh::compose::field_path{.text = "input",
                                            .segments = {"input"}};
  first.to_path = wh::compose::field_path{.text = "out.ok", .segments = {"out", "ok"}};

  wh::compose::compiled_field_mapping_rule failing{};
  failing.from_path = wh::compose::field_path{.text = "missing",
                                              .segments = {"missing"}};
  failing.to_path = wh::compose::field_path{.text = "out.fail", .segments = {"out", "fail"}};

  wh::core::run_context context{};
  auto result = wh::compose::apply_field_mappings_in_place(target, {first, failing}, context);
  REQUIRE(result.has_error());
  REQUIRE(result.error() == wh::core::errc::not_found);
  REQUIRE_FALSE(target.contains("out"));
}
