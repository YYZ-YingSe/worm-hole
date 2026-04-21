#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph/snapshot.hpp"

TEST_CASE("graph snapshot types preserve compile visible metadata",
          "[UT][wh/compose/graph/snapshot.hpp][graph_snapshot][condition][branch][boundary]") {
  wh::compose::graph_snapshot snapshot{};
  snapshot.compile_options.name = "demo";
  snapshot.node_key_to_id.emplace("worker", 1U);
  snapshot.node_id_to_key.push_back("worker");
  snapshot.nodes.push_back({.key = "worker", .node_id = 1U});
  snapshot.edges.push_back({.from = "a", .to = "b"});
  snapshot.branches.push_back({.from = "route", .end_nodes = {"left", "right"}});

  REQUIRE(snapshot.compile_options.name == "demo");
  REQUIRE(snapshot.node_key_to_id.at("worker") == 1U);
  REQUIRE(snapshot.nodes.front().key == "worker");
  REQUIRE(snapshot.edges.front().to == "b");
  REQUIRE(snapshot.branches.front().end_nodes.size() == 2U);
}

TEST_CASE(
    "graph snapshot compile options default to stable graph metadata and empty optional timeout",
    "[UT][wh/compose/graph/"
    "snapshot.hpp][graph_snapshot_compile_options][condition][branch][boundary]") {
  wh::compose::graph_snapshot snapshot{};

  REQUIRE(snapshot.compile_options.name == "graph");
  REQUIRE(snapshot.compile_options.mode == wh::compose::graph_runtime_mode::dag);
  REQUIRE(snapshot.compile_options.max_steps == 1024U);
  REQUIRE(snapshot.compile_options.retain_cold_data);
  REQUIRE_FALSE(snapshot.compile_options.node_timeout.has_value());
  REQUIRE(snapshot.subgraphs.empty());
}
