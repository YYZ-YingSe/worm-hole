#include <catch2/catch_test_macros.hpp>

#include <type_traits>
#include <utility>

#include "wh/embedding/callback_event.hpp"

static_assert(
    std::is_default_constructible_v<wh::embedding::embedding_callback_event>);
static_assert(std::is_move_constructible_v<wh::embedding::embedding_callback_event>);

TEST_CASE("embedding callback event defaults model usage and batch metadata",
          "[UT][wh/embedding/callback_event.hpp][embedding_callback_event][condition][boundary]") {
  wh::embedding::embedding_callback_event event{};

  REQUIRE(event.model_id.empty());
  REQUIRE(event.batch_size == 0U);
  REQUIRE(event.usage.prompt_tokens == 0);
  REQUIRE(event.usage.total_tokens == 0);
}

TEST_CASE("embedding callback event preserves aggregate token usage through copy and move",
          "[UT][wh/embedding/callback_event.hpp][embedding_callback_event][condition][branch][boundary]") {
  const wh::embedding::embedding_callback_event source{
      .model_id = "embed-v1",
      .usage = wh::schema::token_usage{.prompt_tokens = 10,
                                       .completion_tokens = 0,
                                       .total_tokens = 10},
      .batch_size = 3U};
  const auto copied = source;
  auto moved = std::move(copied);

  REQUIRE(source.model_id == "embed-v1");
  REQUIRE(source.usage.prompt_tokens == 10);
  REQUIRE(source.usage.total_tokens == 10);
  REQUIRE(source.batch_size == 3U);
  REQUIRE(moved.model_id == "embed-v1");
  REQUIRE(moved.usage.prompt_tokens == 10);
  REQUIRE(moved.batch_size == 3U);
}
