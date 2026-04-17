#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph/diff_check.hpp"

TEST_CASE("graph diff check reports changed compile options and nodes",
          "[UT][wh/compose/graph/diff_check.hpp][diff_graph][condition][branch][boundary]") {
  wh::compose::graph_snapshot baseline{};
  baseline.compile_options.name = "baseline";
  baseline.nodes.push_back(
      {.key = "a",
       .node_id = 1U,
       .options = {.sync_dispatch = wh::compose::sync_dispatch::work}});

  wh::compose::graph_snapshot candidate = baseline;
  candidate.compile_options.name = "candidate";
  candidate.nodes.push_back({.key = "b", .node_id = 2U});
  candidate.nodes.front().options.sync_dispatch =
      wh::compose::sync_dispatch::inline_control;

  auto diff = wh::compose::diff_graph(baseline, candidate);
  REQUIRE(diff.contains(wh::compose::graph_diff_kind::compile_option));
  REQUIRE(diff.contains(wh::compose::graph_diff_kind::node_added));
  REQUIRE(diff.contains(wh::compose::graph_diff_kind::node_policy));
  REQUIRE(diff.count(wh::compose::graph_diff_kind::compile_option) == 1U);
  REQUIRE_FALSE(diff.kinds().empty());
}

TEST_CASE("graph diff helper methods preserve first-seen kind order and empty counts",
          "[UT][wh/compose/graph/diff_check.hpp][graph_diff::kinds][condition][branch][boundary]") {
  wh::compose::graph_diff diff{};
  REQUIRE(diff.kinds().empty());
  REQUIRE_FALSE(diff.contains(wh::compose::graph_diff_kind::edge_added));
  REQUIRE(diff.count(wh::compose::graph_diff_kind::edge_added) == 0U);

  diff.entries.push_back({.kind = wh::compose::graph_diff_kind::node_removed});
  diff.entries.push_back({.kind = wh::compose::graph_diff_kind::node_removed});
  diff.entries.push_back({.kind = wh::compose::graph_diff_kind::edge_added});

  REQUIRE(diff.contains(wh::compose::graph_diff_kind::node_removed));
  REQUIRE(diff.count(wh::compose::graph_diff_kind::node_removed) == 2U);
  REQUIRE(diff.kinds() == std::vector<wh::compose::graph_diff_kind>{
                              wh::compose::graph_diff_kind::node_removed,
                              wh::compose::graph_diff_kind::edge_added,
                          });
}

TEST_CASE("graph diff detail text helpers expose stable symbolic labels",
          "[UT][wh/compose/graph/diff_check.hpp][mode_text][condition][branch][boundary]") {
  REQUIRE(wh::compose::detail::mode_text(wh::compose::graph_runtime_mode::dag) ==
          "dag");
  REQUIRE(wh::compose::detail::mode_text(wh::compose::graph_runtime_mode::pregel) ==
          "pregel");
  REQUIRE(wh::compose::detail::dispatch_policy_text(
              wh::compose::graph_dispatch_policy::same_wave) == "same_wave");
  REQUIRE(wh::compose::detail::trigger_mode_text(
              wh::compose::graph_trigger_mode::all_predecessors) ==
          "all_predecessors");
  REQUIRE(wh::compose::detail::fan_in_policy_text(
              wh::compose::graph_fan_in_policy::require_all_sources) ==
          "require_all_sources");
  REQUIRE(wh::compose::detail::sync_dispatch_text(
              wh::compose::sync_dispatch::work) == "work");
  REQUIRE(wh::compose::detail::sync_dispatch_text(
              wh::compose::sync_dispatch::inline_control) ==
          "inline_control");
}
