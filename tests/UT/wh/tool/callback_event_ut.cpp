#include <catch2/catch_test_macros.hpp>

#include <type_traits>
#include <utility>

#include "wh/tool/callback_event.hpp"

static_assert(std::is_default_constructible_v<wh::tool::tool_callback_event>);
static_assert(std::is_move_constructible_v<wh::tool::tool_callback_event>);

TEST_CASE("tool callback event defaults textual fields and interruption flag",
          "[UT][wh/tool/callback_event.hpp][tool_callback_event][condition][boundary]") {
  wh::tool::tool_callback_event event{};

  REQUIRE(event.tool_name.empty());
  REQUIRE(event.input_json.empty());
  REQUIRE(event.output_text.empty());
  REQUIRE(event.error_context.empty());
  REQUIRE_FALSE(event.interrupted);
}

TEST_CASE("tool callback event aggregate initialization preserves request output and interruption state",
          "[UT][wh/tool/callback_event.hpp][tool_callback_event][condition][branch][boundary]") {
  const wh::tool::tool_callback_event source{
      .tool_name = "search",
      .input_json = R"({"q":"x"})",
      .output_text = "ok",
      .error_context = "timeout",
      .interrupted = true};
  const auto copied = source;
  auto moved = std::move(copied);

  REQUIRE(source.tool_name == "search");
  REQUIRE(source.input_json == R"({"q":"x"})");
  REQUIRE(source.output_text == "ok");
  REQUIRE(source.error_context == "timeout");
  REQUIRE(source.interrupted);
  REQUIRE(moved.tool_name == "search");
  REQUIRE(moved.input_json == R"({"q":"x"})");
  REQUIRE(moved.output_text == "ok");
  REQUIRE(moved.error_context == "timeout");
  REQUIRE(moved.interrupted);
}
