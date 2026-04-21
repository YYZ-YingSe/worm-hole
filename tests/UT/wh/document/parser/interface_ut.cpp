#include <array>
#include <cstddef>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "wh/document/parser/interface.hpp"

namespace {

struct echo_parser_impl {
  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return {"EchoParser", wh::core::component_kind::document};
  }

  [[nodiscard]] auto parse(const wh::document::parser::parse_request &request) const
      -> wh::core::result<wh::document::document_batch> {
    return wh::document::document_batch{wh::schema::document{request.content}};
  }

  [[nodiscard]] auto parse(wh::document::parser::parse_request &&request) const
      -> wh::core::result<wh::document::document_batch> {
    return wh::document::document_batch{wh::schema::document{std::move(request.content)}};
  }

  [[nodiscard]] auto parse(const wh::document::parser::parse_request_view request) const
      -> wh::core::result<wh::document::document_batch> {
    return wh::document::document_batch{wh::schema::document{std::string{request.content}}};
  }
};

struct heap_parser_impl {
  std::string prefix{};
  std::array<std::byte, 128> padding{};

  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return {"HeapParser", wh::core::component_kind::document};
  }

  [[nodiscard]] auto parse(const wh::document::parser::parse_request &request) const
      -> wh::core::result<wh::document::document_batch> {
    return wh::document::document_batch{wh::schema::document{prefix + ":" + request.content}};
  }

  [[nodiscard]] auto parse(wh::document::parser::parse_request &&request) const
      -> wh::core::result<wh::document::document_batch> {
    return wh::document::document_batch{
        wh::schema::document{prefix + ":" + std::move(request.content)}};
  }

  [[nodiscard]] auto parse(const wh::document::parser::parse_request_view request) const
      -> wh::core::result<wh::document::document_batch> {
    return wh::document::document_batch{
        wh::schema::document{prefix + ":" + std::string{request.content}}};
  }
};

} // namespace

TEST_CASE("parser handle reports empty state and not supported when unset",
          "[UT][wh/document/parser/interface.hpp][parser::parse][branch][boundary]") {
  wh::document::parser::parser parser{};
  REQUIRE_FALSE(parser.has_value());
  REQUIRE(parser.descriptor().type_name.empty());

  wh::document::parser::parse_request request{};
  request.content = "ignored";
  auto status = parser.parse(request);
  REQUIRE(status.has_error());
  REQUIRE(status.error() == wh::core::errc::not_supported);
}

TEST_CASE("parser handle stores copyable parser impl and survives copy and move",
          "[UT][wh/document/parser/interface.hpp][parser][branch]") {
  wh::document::parser::parser parser{echo_parser_impl{}};
  REQUIRE(parser.has_value());
  REQUIRE(parser.descriptor().type_name == "EchoParser");
  REQUIRE(parser.descriptor().kind == wh::core::component_kind::document);

  wh::document::parser::parse_request request{};
  request.content = "copy";
  auto parsed = parser.parse(request);
  REQUIRE(parsed.has_value());
  REQUIRE(parsed.value().front().content() == "copy");

  wh::document::parser::parser copied{parser};
  auto copied_result = copied.parse(request);
  REQUIRE(copied_result.has_value());
  REQUIRE(copied_result.value().front().content() == "copy");

  wh::document::parser::parser moved{std::move(parser)};
  auto moved_result = moved.parse(wh::document::parser::parse_request_view{.content = "view"});
  REQUIRE(moved_result.has_value());
  REQUIRE(moved_result.value().front().content() == "view");
}

TEST_CASE(
    "parser handle covers heap storage move assignment and reset to empty state",
    "[UT][wh/document/parser/interface.hpp][parser::operator=][condition][branch][boundary]") {
  wh::document::parser::parser parser{heap_parser_impl{.prefix = "heap"}};

  wh::document::parser::parse_request owned_request{};
  owned_request.content = "owned";
  auto owned_result = parser.parse(std::move(owned_request));
  REQUIRE(owned_result.has_value());
  REQUIRE(owned_result.value().front().content() == "heap:owned");

  wh::document::parser::parser moved{};
  moved = std::move(parser);
  REQUIRE_FALSE(parser.has_value());
  REQUIRE(moved.has_value());

  auto view_result = moved.parse(wh::document::parser::parse_request_view{.content = "view"});
  REQUIRE(view_result.has_value());
  REQUIRE(view_result.value().front().content() == "heap:view");

  moved = wh::document::parser::parser{};
  REQUIRE_FALSE(moved.has_value());
  auto empty_result = moved.parse(wh::document::parser::parse_request_view{.content = "ignored"});
  REQUIRE(empty_result.has_error());
  REQUIRE(empty_result.error() == wh::core::errc::not_supported);
}
