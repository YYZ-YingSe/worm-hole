#include <catch2/catch_test_macros.hpp>

#include <type_traits>

#include "wh/compose/authored.hpp"

TEST_CASE("compose authored facade exposes authored builders",
          "[UT][wh/compose/authored.hpp][chain][condition][branch][boundary]") {
  STATIC_REQUIRE(std::is_default_constructible_v<wh::compose::chain>);
  STATIC_REQUIRE(std::is_default_constructible_v<wh::compose::parallel>);
  STATIC_REQUIRE(std::is_default_constructible_v<wh::compose::workflow>);
}

TEST_CASE("compose authored facade also re-exports authored branch builders",
          "[UT][wh/compose/authored.hpp][value_branch][condition][branch][boundary]") {
  STATIC_REQUIRE(std::is_default_constructible_v<wh::compose::value_branch>);
  STATIC_REQUIRE(std::is_default_constructible_v<wh::compose::stream_branch>);

  wh::compose::value_branch value{};
  wh::compose::stream_branch stream{};
  REQUIRE(value.end_nodes().empty());
  REQUIRE(stream.end_nodes().empty());
}

TEST_CASE("compose authored facade exposes empty authored builders with predictable defaults",
          "[UT][wh/compose/authored.hpp][workflow][condition][branch][boundary]") {
  wh::compose::chain chain{};
  wh::compose::parallel parallel{};
  wh::compose::workflow workflow{};

  REQUIRE(chain.graph_view().options().mode == wh::compose::graph_runtime_mode::dag);
  REQUIRE(parallel.nodes().empty());
  REQUIRE_FALSE(workflow.graph_view().compiled());
}
