#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph/checkpoint_state.hpp"

TEST_CASE(
    "checkpoint state stores restore and runtime payloads",
    "[UT][wh/compose/graph/checkpoint_state.hpp][checkpoint_state][condition][branch][boundary]") {
  wh::compose::checkpoint_state defaults{};
  REQUIRE(defaults.checkpoint_id.empty());
  REQUIRE(defaults.branch == "main");
  REQUIRE_FALSE(defaults.parent_branch.has_value());
  REQUIRE(defaults.runtime.step_count == 0U);
  REQUIRE(defaults.runtime.lifecycle.empty());
  REQUIRE_FALSE(defaults.runtime.dag.has_value());
  REQUIRE_FALSE(defaults.runtime.pregel.has_value());

  wh::compose::checkpoint_state state{};
  state.checkpoint_id = "cp-1";
  state.branch = "feature";
  state.parent_branch = "main";
  state.runtime.step_count = 5U;
  state.runtime.dag = wh::compose::checkpoint_dag_runtime_state{};
  state.runtime.dag->pending_inputs.entry = wh::compose::graph_value{7};
  state.runtime.dag->pending_inputs.nodes.push_back(wh::compose::checkpoint_node_input{
      .node_id = 3U,
      .key = "node",
      .input = wh::compose::graph_value{9},
  });

  REQUIRE(state.checkpoint_id == "cp-1");
  REQUIRE(state.branch == "feature");
  REQUIRE(state.parent_branch == std::optional<std::string>{"main"});
  REQUIRE(state.runtime.dag.has_value());
  REQUIRE(*wh::core::any_cast<int>(&*state.runtime.dag->pending_inputs.entry) == 7);
  REQUIRE(state.runtime.step_count == 5U);
  REQUIRE(state.runtime.dag->pending_inputs.nodes.size() == 1U);
  REQUIRE(state.runtime.dag->pending_inputs.nodes.front().node_id == 3U);
  REQUIRE(state.runtime.dag->pending_inputs.nodes.front().key == "node");
  REQUIRE(*wh::core::any_cast<int>(&state.runtime.dag->pending_inputs.nodes.front().input) == 9);
}

TEST_CASE(
    "checkpoint state retains runtime and restore payload aggregates",
    "[UT][wh/compose/graph/checkpoint_state.hpp][checkpoint_state][condition][branch][boundary]") {
  wh::compose::checkpoint_state state{};
  state.restore_shape.nodes.push_back(wh::compose::graph_restore_node{
      .key = "a",
  });
  state.restore_shape.nodes.push_back(wh::compose::graph_restore_node{
      .key = "b",
  });
  state.runtime.lifecycle.push_back(wh::compose::graph_node_state{
      .key = "a",
      .node_id = 1U,
      .lifecycle = wh::compose::graph_node_lifecycle_state::completed,
      .attempts = 2U,
      .last_error = wh::core::errc::timeout,
  });
  state.runtime.dag = wh::compose::checkpoint_dag_runtime_state{
      .current_frontier = {1U, 2U},
      .next_frontier = {3U},
      .current_frontier_head = 1U,
  };
  state.runtime.pregel = wh::compose::checkpoint_pregel_runtime_state{
      .current_frontier = {4U},
      .next_frontier = {5U, 6U},
      .current_superstep_active = true,
  };

  REQUIRE(state.restore_shape.nodes.size() == 2U);
  REQUIRE(state.restore_shape.nodes[0].key == "a");
  REQUIRE(state.restore_shape.nodes[1].key == "b");
  REQUIRE(state.runtime.lifecycle.size() == 1U);
  REQUIRE(state.runtime.lifecycle.front().key == "a");
  REQUIRE(state.runtime.lifecycle.front().node_id == 1U);
  REQUIRE(state.runtime.lifecycle.front().lifecycle ==
          wh::compose::graph_node_lifecycle_state::completed);
  REQUIRE(state.runtime.lifecycle.front().attempts == 2U);
  REQUIRE(state.runtime.lifecycle.front().last_error ==
          std::optional<wh::core::error_code>{wh::core::errc::timeout});
  REQUIRE(state.runtime.dag.has_value());
  REQUIRE(state.runtime.dag->current_frontier == std::vector<std::uint32_t>{1U, 2U});
  REQUIRE(state.runtime.dag->next_frontier == std::vector<std::uint32_t>{3U});
  REQUIRE(state.runtime.dag->current_frontier_head == 1U);
  REQUIRE(state.runtime.pregel.has_value());
  REQUIRE(state.runtime.pregel->current_frontier == std::vector<std::uint32_t>{4U});
  REQUIRE(state.runtime.pregel->next_frontier == std::vector<std::uint32_t>{5U, 6U});
  REQUIRE(state.runtime.pregel->current_superstep_active);
}

TEST_CASE(
    "checkpoint state preserves resume and interrupt snapshots independently of node inputs",
    "[UT][wh/compose/graph/checkpoint_state.hpp][checkpoint_state][condition][branch][boundary]") {
  wh::compose::checkpoint_state state{};
  REQUIRE(state.resume_snapshot.upsert("interrupt-1", wh::core::address{"graph", "worker"}, 7)
              .has_value());
  state.interrupt_snapshot.interrupt_id_to_address.emplace("interrupt-1",
                                                           wh::core::address{"graph", "worker"});
  state.runtime.pregel = wh::compose::checkpoint_pregel_runtime_state{};
  state.runtime.pregel->pending_inputs.nodes.push_back(wh::compose::checkpoint_node_input{
      .node_id = 7U,
      .key = "worker",
      .input = wh::compose::graph_value{9},
  });

  auto resume_value = state.resume_snapshot.peek<int>("interrupt-1");
  REQUIRE(resume_value.has_value());
  REQUIRE(resume_value->get() == 7);
  REQUIRE(state.interrupt_snapshot.interrupt_id_to_address.contains("interrupt-1"));
  REQUIRE(state.runtime.pregel.has_value());
  REQUIRE(*wh::core::any_cast<int>(&state.runtime.pregel->pending_inputs.nodes.front().input) == 9);
}
