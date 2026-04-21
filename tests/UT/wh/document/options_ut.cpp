#include <string>

#include <catch2/catch_test_macros.hpp>

#include "wh/document/options.hpp"

namespace {

struct document_options_probe {
  int value{0};
};

} // namespace

TEST_CASE("loader options resolve view and merged values from base and override",
          "[UT][wh/document/options.hpp][loader_options::resolve][branch][boundary]") {
  wh::document::loader_options options{};
  wh::document::loader_common_options base{};
  base.parser.uri = "base://uri";
  base.parser.extra_meta.insert_or_assign("lang", "zh");
  base.parser.extra_meta.insert_or_assign("tenant", "a");
  base.parser.format_options.insert_or_assign("mode", "fast");
  options.set_base(base);

  auto base_view = options.resolve_view();
  REQUIRE(base_view.base_parser != nullptr);
  REQUIRE(base_view.override_parser == nullptr);
  REQUIRE(base_view.parser_uri() == "base://uri");
  REQUIRE(base_view.materialize_parser_extra_meta().at("lang") == "zh");
  REQUIRE(base_view.materialize_parser_format_options().at("mode") == "fast");

  wh::document::loader_common_options override{};
  override.parser.uri = "override://uri";
  override.parser.extra_meta.insert_or_assign("tenant", "b");
  override.parser.extra_meta.insert_or_assign("region", "cn");
  override.parser.format_options.insert_or_assign("mode", "safe");
  override.parser.format_options.insert_or_assign("encoding", "utf-8");
  options.set_call_override(std::move(override));

  auto view = options.resolve_view();
  REQUIRE(view.base_parser != nullptr);
  REQUIRE(view.override_parser != nullptr);
  REQUIRE(view.parser_uri() == "override://uri");

  const auto merged_meta = view.materialize_parser_extra_meta();
  REQUIRE(merged_meta.at("lang") == "zh");
  REQUIRE(merged_meta.at("tenant") == "b");
  REQUIRE(merged_meta.at("region") == "cn");

  const auto merged_format = view.materialize_parser_format_options();
  REQUIRE(merged_format.at("mode") == "safe");
  REQUIRE(merged_format.at("encoding") == "utf-8");

  const auto resolved = options.resolve();
  REQUIRE(resolved.parser.uri == "override://uri");
  REQUIRE(resolved.parser.extra_meta.at("lang") == "zh");
  REQUIRE(resolved.parser.extra_meta.at("tenant") == "b");
  REQUIRE(resolved.parser.format_options.at("mode") == "safe");
}

TEST_CASE("loader options keep base uri when override uri is empty and expose impl extras",
          "[UT][wh/document/options.hpp][loader_options::component_options][branch]") {
  wh::document::loader_options options{};
  wh::document::loader_common_options base{};
  base.parser.uri = "base://uri";
  options.set_base(base);

  wh::document::loader_common_options override{};
  override.parser.extra_meta.insert_or_assign("x", "1");
  options.set_call_override(std::move(override));

  const auto view = options.resolve_view();
  REQUIRE(view.parser_uri() == "base://uri");

  options.component_options().set_impl_specific(document_options_probe{7});
  const auto *probe = options.component_options().impl_specific_if<document_options_probe>();
  REQUIRE(probe != nullptr);
  REQUIRE(probe->value == 7);
}

TEST_CASE("loader options keep base parser state when no override is present",
          "[UT][wh/document/options.hpp][loader_options::resolve_view][condition][boundary]") {
  wh::document::loader_options options{};
  wh::document::loader_common_options base{};
  base.parser.uri = "base://uri";
  base.parser.extra_meta.insert_or_assign("lang", "zh");
  options.set_base(base);

  const auto view = options.resolve_view();
  REQUIRE(view.base_parser != nullptr);
  REQUIRE(view.override_parser == nullptr);
  REQUIRE(view.parser_uri() == "base://uri");
  REQUIRE(view.materialize_parser_extra_meta().at("lang") == "zh");
  REQUIRE(view.materialize_parser_format_options().empty());
}
