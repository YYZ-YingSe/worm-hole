#include <catch2/catch_test_macros.hpp>

#include <type_traits>
#include <utility>

#include "wh/document/callback_event_loader.hpp"

static_assert(std::is_default_constructible_v<wh::document::loader_callback_event>);
static_assert(std::is_move_constructible_v<wh::document::loader_callback_event>);

TEST_CASE("loader callback event defaults to empty uri and zero loaded bytes",
          "[UT][wh/document/callback_event_loader.hpp][loader_callback_event][condition][boundary]") {
  wh::document::loader_callback_event event{};

  REQUIRE(event.uri.empty());
  REQUIRE(event.loaded_bytes == 0U);
}

TEST_CASE("loader callback event aggregate initialization survives copy and move",
          "[UT][wh/document/callback_event_loader.hpp][loader_callback_event][condition][branch][boundary]") {
  const wh::document::loader_callback_event source{
      .uri = "file:///tmp/a.txt", .loaded_bytes = 42U};
  const auto copied = source;
  auto moved = std::move(copied);

  REQUIRE(source.uri == "file:///tmp/a.txt");
  REQUIRE(source.loaded_bytes == 42U);
  REQUIRE(moved.uri == "file:///tmp/a.txt");
  REQUIRE(moved.loaded_bytes == 42U);
}
