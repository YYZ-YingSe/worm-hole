#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "wh/compose/field.hpp"

TEST_CASE("compose field facade parses valid and invalid field paths and compiles mapping rules",
          "[UT][wh/compose/field.hpp][parse_field_path][condition][branch][boundary]") {
  auto parsed = wh::compose::parse_field_path("a.b");
  REQUIRE(parsed.has_value());
  REQUIRE(parsed.value().segments == std::vector<std::string>{"a", "b"});

  auto invalid = wh::compose::parse_field_path("a..b");
  REQUIRE(invalid.has_error());
  REQUIRE(invalid.error() == wh::core::errc::invalid_argument);

  wh::compose::field_mapping_rule static_rule{
      .to_path = "out.answer",
      .static_value = wh::compose::graph_value{7},
  };
  auto compiled_static = wh::compose::compile_field_mapping_rule(static_rule);
  REQUIRE(compiled_static.has_value());
  REQUIRE_FALSE(compiled_static.value().from_path.has_value());
  REQUIRE(compiled_static.value().to_path.segments == std::vector<std::string>{"out", "answer"});

  wh::compose::field_mapping_rule extractor_rule{
      .to_path = "out.answer",
      .extractor = [](const wh::compose::graph_value_map &, wh::core::run_context &)
          -> wh::core::result<wh::compose::graph_value> { return wh::compose::graph_value{9}; },
  };
  auto compiled_extractor = wh::compose::compile_field_mapping_rule(extractor_rule);
  REQUIRE(compiled_extractor.has_value());
  REQUIRE_FALSE(compiled_extractor.value().from_path.has_value());
  REQUIRE(static_cast<bool>(compiled_extractor.value().extractor));
}

TEST_CASE("compose field facade applies nested mappings static values and skip-missing rules",
          "[UT][wh/compose/field.hpp][apply_field_mappings][condition][branch][boundary]") {
  wh::compose::graph_value_map source{};
  source.insert_or_assign("user", wh::compose::graph_value{wh::compose::graph_value_map{
                                      {"id", wh::compose::graph_value{42}},
                                  }});

  std::vector<wh::compose::field_mapping_rule> rules{
      wh::compose::field_mapping_rule{
          .from_path = "user.id",
          .to_path = "input.user_id",
      },
      wh::compose::field_mapping_rule{
          .from_path = "missing.value",
          .to_path = "ignored.value",
          .missing_policy = wh::compose::field_missing_policy::skip,
      },
      wh::compose::field_mapping_rule{
          .to_path = "meta.label",
          .static_value = wh::compose::graph_value{std::string{"ok"}},
      },
      wh::compose::field_mapping_rule{
          .to_path = "meta.extracted",
          .extractor = [](const wh::compose::graph_value_map &map,
                          wh::core::run_context &) -> wh::core::result<wh::compose::graph_value> {
            const auto *user = wh::core::any_cast<wh::compose::graph_value_map>(&map.at("user"));
            if (user == nullptr) {
              return wh::core::result<wh::compose::graph_value>::failure(
                  wh::core::errc::type_mismatch);
            }
            return user->at("id");
          },
      },
  };

  wh::core::run_context context{};
  auto updated =
      wh::compose::apply_field_mappings(source, wh::compose::graph_value_map{}, rules, context);

  REQUIRE(updated.has_value());
  auto &mapped = updated.value();
  auto *input = wh::core::any_cast<wh::compose::graph_value_map>(&mapped.at("input"));
  REQUIRE(input != nullptr);
  REQUIRE(*wh::core::any_cast<int>(&input->at("user_id")) == 42);

  auto *meta = wh::core::any_cast<wh::compose::graph_value_map>(&mapped.at("meta"));
  REQUIRE(meta != nullptr);
  REQUIRE(*wh::core::any_cast<std::string>(&meta->at("label")) == "ok");
  REQUIRE(*wh::core::any_cast<int>(&meta->at("extracted")) == 42);
  REQUIRE(mapped.find("ignored") == mapped.end());
}

TEST_CASE("compose field facade reports missing-source failures when skip is not enabled",
          "[UT][wh/compose/field.hpp][apply_field_mappings_in_place][branch]") {
  wh::compose::graph_value_map target{};
  std::vector<wh::compose::compiled_field_mapping_rule> rules{
      wh::compose::compiled_field_mapping_rule{
          .from_path =
              wh::compose::field_path{
                  .text = "missing.value",
                  .segments = {"missing", "value"},
              },
          .to_path =
              wh::compose::field_path{
                  .text = "output.value",
                  .segments = {"output", "value"},
              },
      },
  };

  wh::core::run_context context{};
  auto status = wh::compose::apply_field_mappings_in_place(target, rules, context);
  REQUIRE(status.has_error());
  REQUIRE(status.error() == wh::core::errc::not_found);
}
