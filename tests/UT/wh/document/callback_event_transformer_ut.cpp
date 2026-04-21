#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "wh/document/callback_event_transformer.hpp"

static_assert(std::is_default_constructible_v<wh::document::transformer_callback_event>);
static_assert(std::is_copy_constructible_v<wh::document::transformer_callback_event>);

TEST_CASE("transformer callback event defaults both counters to zero",
          "[UT][wh/document/"
          "callback_event_transformer.hpp][transformer_callback_event][condition][boundary]") {
  wh::document::transformer_callback_event event{};

  REQUIRE(event.input_count == 0U);
  REQUIRE(event.output_count == 0U);
}

TEST_CASE(
    "transformer callback event preserves aggregate counts across copy and assignment",
    "[UT][wh/document/"
    "callback_event_transformer.hpp][transformer_callback_event][condition][branch][boundary]") {
  const wh::document::transformer_callback_event source{.input_count = 5U, .output_count = 7U};
  auto copied = source;
  wh::document::transformer_callback_event assigned{};
  assigned = copied;

  REQUIRE(copied.input_count == 5U);
  REQUIRE(copied.output_count == 7U);
  REQUIRE(assigned.input_count == 5U);
  REQUIRE(assigned.output_count == 7U);
}
