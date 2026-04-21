#include <string>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "wh/document/parser/ext_parser.hpp"

namespace {

struct maybe_empty_parser {
  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return {"MaybeEmptyParser", wh::core::component_kind::document};
  }

  [[nodiscard]] auto parse(const wh::document::parser::parse_request &request) const
      -> wh::core::result<wh::document::document_batch> {
    if (request.content.empty()) {
      return wh::document::document_batch{wh::schema::document{""}};
    }
    return wh::document::document_batch{wh::schema::document{request.content}};
  }

  [[nodiscard]] auto parse(wh::document::parser::parse_request &&request) const
      -> wh::core::result<wh::document::document_batch> {
    return parse(static_cast<const wh::document::parser::parse_request &>(request));
  }

  [[nodiscard]] auto parse(const wh::document::parser::parse_request_view request) const
      -> wh::core::result<wh::document::document_batch> {
    if (request.content.empty()) {
      return wh::document::document_batch{wh::schema::document{""}};
    }
    return wh::document::document_batch{wh::schema::document{std::string{request.content}}};
  }
};

} // namespace

TEST_CASE(
    "ext parser validates registrations normalizes extensions and exposes registry copy",
    "[UT][wh/document/parser/ext_parser.hpp][ext_parser::register_parser][branch][boundary]") {
  wh::document::parser::ext_parser parser{};

  auto empty_ext = parser.register_parser("", wh::document::parser::make_text_parser());
  REQUIRE(empty_ext.has_error());
  REQUIRE(empty_ext.error() == wh::core::errc::invalid_argument);

  auto registered = parser.register_parser(".txt", wh::document::parser::make_text_parser());
  REQUIRE(registered.has_value());

  const auto registry = parser.parser_registry_copy();
  REQUIRE(registry.contains("txt"));
}

TEST_CASE("ext parser routes by extension strips empty documents and falls back",
          "[UT][wh/document/parser/ext_parser.hpp][ext_parser::parse][branch]") {
  wh::document::parser::ext_parser parser{};
  REQUIRE(parser.register_parser(".empty", wh::document::parser::parser{maybe_empty_parser{}})
              .has_value());

  wh::document::parser::parse_request empty_request{};
  empty_request.content = "";
  empty_request.options.uri = "doc.empty";
  auto empty_result = parser.parse(empty_request);
  REQUIRE(empty_result.has_value());
  REQUIRE(empty_result.value().empty());

  parser.clear_fallback();
  wh::document::parser::parse_request missing_request{};
  missing_request.content = "hello";
  missing_request.options.uri = "doc.unknown";
  auto missing = parser.parse(missing_request);
  REQUIRE(missing.has_error());
  REQUIRE(missing.error() == wh::core::errc::not_found);

  parser.set_fallback(wh::document::parser::make_text_parser());
  auto fallback = parser.parse(wh::document::parser::parse_request_view{
      .content = "hello",
      .options = {.uri = "doc.unknown"},
  });
  REQUIRE(fallback.has_value());
  REQUIRE(fallback.value().size() == 1U);
  REQUIRE(fallback.value().front().content() == "hello");
}

TEST_CASE(
    "ext parser rejects empty parser handles and treats engaged empty fallback as not supported",
    "[UT][wh/document/parser/"
    "ext_parser.hpp][ext_parser::set_fallback][condition][branch][boundary]") {
  wh::document::parser::ext_parser parser{};

  auto invalid = parser.register_parser(".bad", wh::document::parser::parser{});
  REQUIRE(invalid.has_error());
  REQUIRE(invalid.error() == wh::core::errc::invalid_argument);

  REQUIRE(
      parser.register_parser(std::string{"md"}, wh::document::parser::parser{maybe_empty_parser{}})
          .has_value());

  wh::document::parser::parse_request owned_request{};
  owned_request.content = "owned";
  owned_request.options.uri = "docs/note.md";
  auto owned = parser.parse(std::move(owned_request));
  REQUIRE(owned.has_value());
  REQUIRE(owned.value().size() == 1U);
  REQUIRE(owned.value().front().content() == "owned");

  parser.set_fallback(wh::document::parser::parser{});
  wh::document::parser::parse_request missing_request{};
  missing_request.content = "missing";
  missing_request.options.uri = "docs/note.unknown";
  auto unsupported = parser.parse(std::move(missing_request));
  REQUIRE(unsupported.has_error());
  REQUIRE(unsupported.error() == wh::core::errc::not_supported);
}
