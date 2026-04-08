#include <catch2/catch_test_macros.hpp>

#include "helper/compose_graph_runtime_support.hpp"
#include "wh/compose/graph/detail/runtime/dag_run_state.hpp"

TEST_CASE("dag run state sizes branch storage and seeds frontier from start entry",
          "[UT][wh/compose/graph/detail/runtime/dag_run_state.hpp][dag_run_state::initialize_dag_entry][condition][branch][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::dag, "dag_runtime");
  REQUIRE(graph.has_value());

  wh::core::run_context context{};
  auto base = wh::testing::helper::make_base_run_state(
      graph.value(), wh::compose::graph_value{13}, context);
  wh::compose::detail::invoke_runtime::dag_run_state dag_state{std::move(base)};

  REQUIRE(dag_state.branch_states().size() >= graph->snapshot_view().nodes.size());
  dag_state.initialize_dag_entry();

  auto worker_id = graph->node_id("worker");
  REQUIRE(worker_id.has_value());
  REQUIRE(dag_state.frontier().dequeue() ==
          std::optional<std::uint32_t>{worker_id.value()});
  REQUIRE_FALSE(dag_state.promote_next_wave());
}

TEST_CASE("dag run state starts with an empty frontier and clears injected start selections after init",
          "[UT][wh/compose/graph/detail/runtime/dag_run_state.hpp][dag_run_state::frontier][condition][branch][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::dag, "dag_runtime_probe");
  REQUIRE(graph.has_value());

  auto worker_id = graph->node_id("worker");
  REQUIRE(worker_id.has_value());

  wh::core::run_context context{};
  auto base = wh::testing::helper::make_base_run_state(
      graph.value(), wh::compose::graph_value{17}, context);
  wh::compose::detail::invoke_runtime::dag_run_state dag_state{std::move(base)};

  REQUIRE_FALSE(dag_state.frontier().dequeue().has_value());
  dag_state.initialize_dag_entry();

  REQUIRE(dag_state.frontier().dequeue() ==
          std::optional<std::uint32_t>{worker_id.value()});
}
