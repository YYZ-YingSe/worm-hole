#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph/restore_shape.hpp"

TEST_CASE(
    "restore shape conversion sorts nodes edges branches and subgraphs",
    "[UT][wh/compose/graph/restore_shape.hpp][to_restore_shape][condition][branch][boundary]") {
  wh::compose::graph_snapshot snapshot{};
  snapshot.compile_options.mode = wh::compose::graph_runtime_mode::pregel;
  snapshot.nodes = {
      {.key = "z", .node_id = 2U},
      {.key = "a", .node_id = 1U},
  };
  snapshot.edges = {
      {.from = "z", .to = "a"},
      {.from = "a", .to = "z"},
  };
  snapshot.branches = {
      {.from = "route", .end_nodes = {"right", "left"}},
  };
  snapshot.subgraphs.emplace("child", wh::compose::graph_snapshot{});

  auto shape = wh::compose::detail::to_restore_shape(snapshot);
  REQUIRE(shape.options.mode == wh::compose::graph_runtime_mode::pregel);
  REQUIRE(shape.nodes.front().key == "a");
  REQUIRE(shape.edges.front().from == "a");
  REQUIRE(shape.branches.front().end_nodes == std::vector<std::string>{"left", "right"});
  REQUIRE(shape.subgraphs.contains("child"));
}

TEST_CASE(
    "restore shape defaults preserve dag options and empty collections",
    "[UT][wh/compose/graph/restore_shape.hpp][graph_restore_shape][condition][branch][boundary]") {
  wh::compose::graph_restore_shape shape{};

  REQUIRE(shape.options.mode == wh::compose::graph_runtime_mode::dag);
  REQUIRE(shape.options.dispatch_policy == wh::compose::graph_dispatch_policy::same_wave);
  REQUIRE(shape.nodes.empty());
  REQUIRE(shape.edges.empty());
  REQUIRE(shape.branches.empty());
  REQUIRE(shape.subgraphs.empty());
}
