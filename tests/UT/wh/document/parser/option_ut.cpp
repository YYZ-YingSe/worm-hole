#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "wh/document/parser/option.hpp"

TEST_CASE("parse options view iterates base and override metadata entries",
          "[UT][wh/document/parser/"
          "option.hpp][parse_options_view::for_each_extra_meta][branch][boundary]") {
  wh::document::parser::parser_string_map base{
      {"lang", "zh"},
      {"tenant", "base"},
  };
  wh::document::parser::parser_string_map override{
      {"tenant", "override"},
      {"region", "cn"},
  };

  wh::document::parser::parse_options_view view{};
  view.extra_meta_base = &base;
  view.extra_meta_override = &override;

  std::vector<std::pair<std::string, std::string>> seen{};
  view.for_each_extra_meta(
      [&](const std::string &key, const std::string &value) { seen.emplace_back(key, value); });
  std::sort(seen.begin(), seen.end());

  REQUIRE(seen == std::vector<std::pair<std::string, std::string>>{
                      {"lang", "zh"},
                      {"region", "cn"},
                      {"tenant", "base"},
                      {"tenant", "override"},
                  });
}

TEST_CASE("parse options view iterates format options and tolerates null maps",
          "[UT][wh/document/parser/"
          "option.hpp][parse_options_view::for_each_format_option][branch][boundary]") {
  wh::document::parser::parse_options_view empty{};
  std::size_t empty_count = 0U;
  empty.for_each_format_option([&](const auto &, const auto &) { ++empty_count; });
  REQUIRE(empty_count == 0U);

  wh::document::parser::parser_string_map base{
      {"mode", "fast"},
  };
  wh::document::parser::parser_string_map override{
      {"encoding", "utf-8"},
  };

  wh::document::parser::parse_options_view view{};
  view.format_options_base = &base;
  view.format_options_override = &override;

  std::vector<std::pair<std::string, std::string>> seen{};
  view.for_each_format_option(
      [&](const std::string &key, const std::string &value) { seen.emplace_back(key, value); });
  std::sort(seen.begin(), seen.end());

  REQUIRE(seen == std::vector<std::pair<std::string, std::string>>{
                      {"encoding", "utf-8"},
                      {"mode", "fast"},
                  });
}

TEST_CASE("parse options view supports single-source overlays and preserves plain option fields",
          "[UT][wh/document/parser/option.hpp][parse_options_view][condition][branch][boundary]") {
  wh::document::parser::parse_options options{};
  options.uri = "doc://plain";
  options.extra_meta.insert_or_assign("tenant", "alpha");
  options.format_options.insert_or_assign("mode", "strict");

  REQUIRE(options.uri == "doc://plain");
  REQUIRE(options.extra_meta.at("tenant") == "alpha");
  REQUIRE(options.format_options.at("mode") == "strict");

  wh::document::parser::parse_options_view view{};
  view.uri = options.uri;
  view.extra_meta_override = &options.extra_meta;
  view.format_options_base = &options.format_options;

  std::vector<std::pair<std::string, std::string>> extra_seen{};
  view.for_each_extra_meta([&](const std::string &key, const std::string &value) {
    extra_seen.emplace_back(key, value);
  });
  REQUIRE(extra_seen == std::vector<std::pair<std::string, std::string>>{{"tenant", "alpha"}});

  std::vector<std::pair<std::string, std::string>> format_seen{};
  view.for_each_format_option([&](const std::string &key, const std::string &value) {
    format_seen.emplace_back(key, value);
  });
  REQUIRE(format_seen == std::vector<std::pair<std::string, std::string>>{{"mode", "strict"}});
}
