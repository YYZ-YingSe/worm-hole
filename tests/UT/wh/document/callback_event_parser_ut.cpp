#include <type_traits>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "wh/document/callback_event_parser.hpp"

static_assert(std::is_default_constructible_v<wh::document::parser_callback_event>);
static_assert(std::is_move_constructible_v<wh::document::parser_callback_event>);

TEST_CASE(
    "parser callback event defaults to empty uri and zero counters",
    "[UT][wh/document/callback_event_parser.hpp][parser_callback_event][condition][boundary]") {
  wh::document::parser_callback_event event{};

  REQUIRE(event.uri.empty());
  REQUIRE(event.input_bytes == 0U);
  REQUIRE(event.output_count == 0U);
}

TEST_CASE("parser callback event aggregate initialization preserves parser metrics across moves",
          "[UT][wh/document/"
          "callback_event_parser.hpp][parser_callback_event][condition][branch][boundary]") {
  const wh::document::parser_callback_event source{
      .uri = "doc://x", .input_bytes = 128U, .output_count = 3U};
  auto moved = source;
  auto moved_again = std::move(moved);

  REQUIRE(source.uri == "doc://x");
  REQUIRE(source.input_bytes == 128U);
  REQUIRE(source.output_count == 3U);
  REQUIRE(moved_again.uri == "doc://x");
  REQUIRE(moved_again.input_bytes == 128U);
  REQUIRE(moved_again.output_count == 3U);
}
