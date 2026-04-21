#include <catch2/catch_test_macros.hpp>

#include "wh/document/parser/text_parser.hpp"

TEST_CASE("text parser builds one document from owned request and preserves metadata",
          "[UT][wh/document/parser/text_parser.hpp][text_parser::parse][branch][boundary]") {
  wh::document::parser::text_parser parser{};
  wh::document::parser::parse_request request{};
  request.content = "hello";
  request.options.uri = "doc://owned";
  request.options.extra_meta.insert_or_assign("lang", "zh");

  auto parsed = parser.parse(request);
  REQUIRE(parsed.has_value());
  REQUIRE(parsed.value().size() == 1U);
  REQUIRE(parsed.value().front().content() == "hello");
  REQUIRE(parsed.value().front().metadata_or<std::string>("_source") == "doc://owned");
  REQUIRE(parsed.value().front().metadata_or<std::string>("lang") == "zh");
}

TEST_CASE("text parser view path applies override-last metadata semantics",
          "[UT][wh/document/parser/text_parser.hpp][text_parser::parse][branch]") {
  wh::document::parser::text_parser parser{};
  wh::document::parser::parser_string_map base{
      {"lang", "zh"},
      {"tenant", "base"},
  };
  wh::document::parser::parser_string_map override{
      {"tenant", "override"},
  };

  wh::document::parser::parse_request_view request{};
  request.content = "hello-view";
  request.options.uri = "doc://view";
  request.options.extra_meta_base = &base;
  request.options.extra_meta_override = &override;

  auto parsed = parser.parse(request);
  REQUIRE(parsed.has_value());
  REQUIRE(parsed.value().size() == 1U);
  REQUIRE(parsed.value().front().content() == "hello-view");
  REQUIRE(parsed.value().front().metadata_or<std::string>("_source") == "doc://view");
  REQUIRE(parsed.value().front().metadata_or<std::string>("tenant") == "override");
}

TEST_CASE(
    "text parser erased factory preserves owned-request metadata on move path",
    "[UT][wh/document/parser/text_parser.hpp][make_text_parser][condition][branch][boundary]") {
  auto parser = wh::document::parser::make_text_parser();
  REQUIRE(parser.has_value());
  REQUIRE(parser.descriptor().type_name == "TextParser");

  wh::document::parser::parse_request owned_request{};
  owned_request.content = "owned-body";
  owned_request.options.uri = "doc://owned";
  owned_request.options.extra_meta.insert_or_assign("tenant", "alpha");

  auto parsed = parser.parse(std::move(owned_request));
  REQUIRE(parsed.has_value());
  REQUIRE(parsed.value().size() == 1U);
  REQUIRE(parsed.value().front().content() == "owned-body");
  REQUIRE(parsed.value().front().metadata_or<std::string>("_source") == "doc://owned");
  REQUIRE(parsed.value().front().metadata_or<std::string>("tenant") == "alpha");
}
