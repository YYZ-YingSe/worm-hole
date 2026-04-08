#include <catch2/catch_test_macros.hpp>

#include <type_traits>

#include "wh/indexer/callback_event.hpp"

static_assert(std::is_default_constructible_v<wh::indexer::indexer_callback_event>);
static_assert(std::is_copy_constructible_v<wh::indexer::indexer_callback_event>);

TEST_CASE("indexer callback event defaults batch success and failure counters to zero",
          "[UT][wh/indexer/callback_event.hpp][indexer_callback_event][condition][boundary]") {
  wh::indexer::indexer_callback_event event{};

  REQUIRE(event.batch_size == 0U);
  REQUIRE(event.success_count == 0U);
  REQUIRE(event.failure_count == 0U);
}

TEST_CASE("indexer callback event aggregate values preserve balanced success and failure accounting",
          "[UT][wh/indexer/callback_event.hpp][indexer_callback_event][condition][branch][boundary]") {
  const wh::indexer::indexer_callback_event source{
      .batch_size = 8U, .success_count = 7U, .failure_count = 1U};
  auto copied = source;

  REQUIRE(copied.batch_size == 8U);
  REQUIRE(copied.success_count == 7U);
  REQUIRE(copied.failure_count == 1U);
  REQUIRE(copied.success_count + copied.failure_count == copied.batch_size);
}
