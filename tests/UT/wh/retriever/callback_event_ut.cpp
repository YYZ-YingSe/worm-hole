#include <catch2/catch_test_macros.hpp>

#include <type_traits>
#include <utility>

#include "wh/retriever/callback_event.hpp"

static_assert(
    std::is_default_constructible_v<wh::retriever::retriever_callback_event>);
static_assert(
    std::is_move_constructible_v<wh::retriever::retriever_callback_event>);

TEST_CASE("retriever callback event defaults top-k threshold and text fields",
          "[UT][wh/retriever/callback_event.hpp][retriever_callback_event][condition][boundary]") {
  wh::retriever::retriever_callback_event event{};

  REQUIRE(event.top_k == 0U);
  REQUIRE(event.score_threshold == 0.0);
  REQUIRE(event.filter.empty());
  REQUIRE(event.extra.empty());
}

TEST_CASE("retriever callback event aggregate initialization preserves selection metadata",
          "[UT][wh/retriever/callback_event.hpp][retriever_callback_event][condition][branch][boundary]") {
  const wh::retriever::retriever_callback_event source{
      .top_k = 4U,
      .score_threshold = 0.25,
      .filter = "dsl=faq",
      .extra = "query"};
  const auto copied = source;
  auto moved = std::move(copied);

  REQUIRE(source.top_k == 4U);
  REQUIRE(source.score_threshold == 0.25);
  REQUIRE(source.filter == "dsl=faq");
  REQUIRE(source.extra == "query");
  REQUIRE(moved.top_k == 4U);
  REQUIRE(moved.score_threshold == 0.25);
  REQUIRE(moved.filter == "dsl=faq");
  REQUIRE(moved.extra == "query");
}
