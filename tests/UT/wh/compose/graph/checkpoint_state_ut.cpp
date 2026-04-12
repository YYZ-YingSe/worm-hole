#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph/checkpoint_state.hpp"

TEST_CASE("checkpoint state stores restore and rerun payloads",
          "[UT][wh/compose/graph/checkpoint_state.hpp][checkpoint_state][condition][branch][boundary]") {
  wh::compose::checkpoint_state defaults{};
  REQUIRE(defaults.checkpoint_id.empty());
  REQUIRE(defaults.branch == "main");
  REQUIRE_FALSE(defaults.parent_branch.has_value());
  REQUIRE(defaults.node_states.empty());
  REQUIRE(defaults.rerun_inputs.empty());

  wh::compose::checkpoint_state state{};
  state.checkpoint_id = "cp-1";
  state.branch = "feature";
  state.parent_branch = "main";
  state.rerun_inputs.insert_or_assign("node", wh::compose::graph_value{7});

  REQUIRE(state.checkpoint_id == "cp-1");
  REQUIRE(state.branch == "feature");
  REQUIRE(state.parent_branch == std::optional<std::string>{"main"});
  REQUIRE(*wh::core::any_cast<int>(&state.rerun_inputs.at("node")) == 7);
}

TEST_CASE("checkpoint state retains runtime and restore payload aggregates",
          "[UT][wh/compose/graph/checkpoint_state.hpp][checkpoint_state][condition][branch][boundary]") {
  wh::compose::checkpoint_state state{};
  state.restore_shape.nodes.push_back(wh::compose::graph_restore_node{
      .key = "a",
  });
  state.restore_shape.nodes.push_back(wh::compose::graph_restore_node{
      .key = "b",
  });
  state.node_states.push_back(wh::compose::graph_node_state{
      .key = "a",
      .node_id = 1U,
      .lifecycle = wh::compose::graph_node_lifecycle_state::completed,
      .attempts = 2U,
      .last_error = wh::core::errc::timeout,
  });

  REQUIRE(state.restore_shape.nodes.size() == 2U);
  REQUIRE(state.restore_shape.nodes[0].key == "a");
  REQUIRE(state.restore_shape.nodes[1].key == "b");
  REQUIRE(state.node_states.size() == 1U);
  REQUIRE(state.node_states.front().key == "a");
  REQUIRE(state.node_states.front().node_id == 1U);
  REQUIRE(state.node_states.front().lifecycle ==
          wh::compose::graph_node_lifecycle_state::completed);
  REQUIRE(state.node_states.front().attempts == 2U);
  REQUIRE(state.node_states.front().last_error ==
          std::optional<wh::core::error_code>{wh::core::errc::timeout});
}

TEST_CASE("checkpoint state preserves resume and interrupt snapshots independently of rerun inputs",
          "[UT][wh/compose/graph/checkpoint_state.hpp][checkpoint_state][condition][branch][boundary]") {
  wh::compose::checkpoint_state state{};
  REQUIRE(state.resume_snapshot
              .upsert("interrupt-1", wh::core::address{"graph", "worker"}, 7)
              .has_value());
  state.interrupt_snapshot.interrupt_id_to_address.emplace(
      "interrupt-1", wh::core::address{"graph", "worker"});
  state.rerun_inputs.insert_or_assign("worker", wh::compose::graph_value{9});

  auto resume_value = state.resume_snapshot.peek<int>("interrupt-1");
  REQUIRE(resume_value.has_value());
  REQUIRE(resume_value->get() == 7);
  REQUIRE(state.interrupt_snapshot.interrupt_id_to_address.contains("interrupt-1"));
  REQUIRE(*wh::core::any_cast<int>(&state.rerun_inputs.at("worker")) == 9);
}
