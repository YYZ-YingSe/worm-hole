#include <catch2/catch_test_macros.hpp>

#include "wh/net/concepts/sse_parser_concept.hpp"

namespace {

struct concept_sse_parser {
  auto parse(const wh::net::sse_parse_request &) const -> wh::net::sse_parse_result;
  auto parse(wh::net::sse_parse_request &&) const -> wh::net::sse_parse_result;
  auto parse(const wh::net::sse_parse_request_view) const -> wh::net::sse_parse_result;
};

struct incomplete_sse_parser {
  auto parse(const wh::net::sse_parse_request &) const -> wh::net::sse_parse_result;
};

static_assert(wh::net::sse_parser_like<concept_sse_parser>);
static_assert(!wh::net::sse_parser_like<incomplete_sse_parser>);

} // namespace

TEST_CASE(
    "sse parser concept accepts complete parser adapters",
    "[UT][wh/net/concepts/sse_parser_concept.hpp][sse_parser_like][condition][branch][boundary]") {
  REQUIRE(wh::net::sse_parser_like<concept_sse_parser>);
  REQUIRE_FALSE(wh::net::sse_parser_like<incomplete_sse_parser>);
}

TEST_CASE("sse parser concept rejects adapters missing borrowed or movable parse overloads",
          "[UT][wh/net/concepts/"
          "sse_parser_concept.hpp][sse_parser_like][condition][branch][boundary][negative]") {
  constexpr bool valid = wh::net::sse_parser_like<concept_sse_parser>;
  constexpr bool invalid = wh::net::sse_parser_like<incomplete_sse_parser>;
  REQUIRE(valid);
  REQUIRE_FALSE(invalid);
}
