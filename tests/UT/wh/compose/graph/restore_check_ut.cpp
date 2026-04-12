#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph/restore_check.hpp"

TEST_CASE("restore check reports shape and checkpoint target mismatches",
          "[UT][wh/compose/graph/restore_check.hpp][validate][condition][branch][boundary]") {
  wh::compose::graph_restore_shape baseline{};
  baseline.nodes.push_back({.key = "worker"});

  wh::compose::graph_restore_shape current{};
  current.nodes.push_back({.key = "other"});

  wh::compose::checkpoint_state checkpoint{};
  checkpoint.restore_shape = baseline;
  checkpoint.node_states.push_back({.key = "ghost"});
  checkpoint.rerun_inputs.insert_or_assign("ghost", wh::compose::graph_value{1});
  REQUIRE(checkpoint.resume_snapshot
              .upsert("interrupt-1", wh::core::address{}.append("graph").append("ghost"),
                      1)
              .has_value());
  checkpoint.interrupt_snapshot.interrupt_id_to_address.insert_or_assign(
      "interrupt-2", wh::core::address{}.append("graph").append("ghost"));

  auto validation = wh::compose::restore_check::validate(current, checkpoint);
  REQUIRE_FALSE(validation.restorable);
  REQUIRE(validation.diff.contains(
      wh::compose::restore_check::restore_diff_kind::node_added));
  REQUIRE_FALSE(validation.issues.empty());
}

TEST_CASE("restore check helper diff counters and text helpers expose stable values",
          "[UT][wh/compose/graph/restore_check.hpp][restore_diff::count][condition][branch][boundary]") {
  wh::compose::restore_check::restore_diff diff{};
  diff.entries.push_back(
      {.kind = wh::compose::restore_check::restore_diff_kind::node_added});
  diff.entries.push_back(
      {.kind = wh::compose::restore_check::restore_diff_kind::node_added});
  diff.entries.push_back(
      {.kind = wh::compose::restore_check::restore_diff_kind::edge_removed});

  REQUIRE(diff.count(wh::compose::restore_check::restore_diff_kind::node_added) ==
          2U);
  REQUIRE(diff.contains(wh::compose::restore_check::restore_diff_kind::edge_removed));
  REQUIRE(wh::compose::restore_check::mode_text(
              wh::compose::graph_runtime_mode::pregel) == "pregel");
  REQUIRE(wh::compose::restore_check::trigger_mode_text(
              wh::compose::graph_trigger_mode::any_predecessor) ==
          "any_predecessor");
}
