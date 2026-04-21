#include <type_traits>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "wh/prompt/callback_event.hpp"

static_assert(std::is_default_constructible_v<wh::prompt::prompt_callback_event>);
static_assert(std::is_move_constructible_v<wh::prompt::prompt_callback_event>);

TEST_CASE("prompt callback event defaults names counts and failure metadata",
          "[UT][wh/prompt/callback_event.hpp][prompt_callback_event][condition][boundary]") {
  wh::prompt::prompt_callback_event event{};

  REQUIRE(event.template_name.empty());
  REQUIRE(event.variable_count == 0U);
  REQUIRE(event.rendered_message_count == 0U);
  REQUIRE(event.failed_template.empty());
  REQUIRE(event.failed_variable.empty());
}

TEST_CASE(
    "prompt callback event aggregate initialization preserves render and failure fields across "
    "moves",
    "[UT][wh/prompt/callback_event.hpp][prompt_callback_event][condition][branch][boundary]") {
  const wh::prompt::prompt_callback_event source{.template_name = "chat",
                                                 .variable_count = 2U,
                                                 .rendered_message_count = 1U,
                                                 .failed_template = "step",
                                                 .failed_variable = "name"};
  const auto copied = source;
  auto moved = std::move(copied);

  REQUIRE(source.template_name == "chat");
  REQUIRE(source.variable_count == 2U);
  REQUIRE(source.rendered_message_count == 1U);
  REQUIRE(source.failed_template == "step");
  REQUIRE(source.failed_variable == "name");
  REQUIRE(moved.template_name == "chat");
  REQUIRE(moved.failed_template == "step");
  REQUIRE(moved.failed_variable == "name");
}
