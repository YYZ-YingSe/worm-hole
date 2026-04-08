#include <catch2/catch_test_macros.hpp>

#include <type_traits>
#include <utility>

#include "wh/model/callback_event.hpp"

static_assert(
    std::is_default_constructible_v<wh::model::chat_model_callback_event>);
static_assert(std::is_move_constructible_v<wh::model::chat_model_callback_event>);

TEST_CASE("chat model callback event defaults to non-stream zero-usage metadata",
          "[UT][wh/model/callback_event.hpp][chat_model_callback_event][condition][boundary]") {
  wh::model::chat_model_callback_event event{};

  REQUIRE(event.model_id.empty());
  REQUIRE(event.emitted_chunks == 0U);
  REQUIRE_FALSE(event.stream_path);
  REQUIRE(event.usage.prompt_tokens == 0);
  REQUIRE(event.usage.total_tokens == 0);
}

TEST_CASE("chat model callback event aggregate initialization preserves usage and stream flags",
          "[UT][wh/model/callback_event.hpp][chat_model_callback_event][condition][branch][boundary]") {
  const wh::model::chat_model_callback_event source{
      .model_id = "gpt-x",
      .usage = wh::schema::token_usage{.prompt_tokens = 12,
                                       .completion_tokens = 8,
                                       .total_tokens = 20},
      .emitted_chunks = 4U,
      .stream_path = true};
  const auto copied = source;
  auto moved = std::move(copied);

  REQUIRE(source.model_id == "gpt-x");
  REQUIRE(source.emitted_chunks == 4U);
  REQUIRE(source.stream_path);
  REQUIRE(source.usage.total_tokens == 20);
  REQUIRE(moved.model_id == "gpt-x");
  REQUIRE(moved.emitted_chunks == 4U);
  REQUIRE(moved.stream_path);
  REQUIRE(moved.usage.prompt_tokens == 12);
}
