#include <catch2/catch_test_macros.hpp>

#include "helper/compose_graph_runtime_support.hpp"
#include "wh/compose/graph/detail/pregel_loop.hpp"
#include "wh/compose/graph/detail/start.hpp"

TEST_CASE("pregel loop prepares launch actions for current frontier nodes",
          "[UT][wh/compose/graph/detail/pregel_loop.hpp][pregel_run_state::take_next_pregel_action][condition][branch][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::pregel, "pregel_loop");
  REQUIRE(graph.has_value());

  wh::core::run_context context{};
  auto base = wh::testing::helper::make_base_run_state(
      graph.value(), wh::compose::graph_value{12}, context);
  wh::compose::detail::invoke_runtime::pregel_run_state pregel_state{
      std::move(base)};
  pregel_state.initialize_pregel_entry();

  auto worker_id = graph->node_id("worker");
  REQUIRE(worker_id.has_value());

  auto action = pregel_state.take_next_pregel_action(worker_id.value(), 1U);
  REQUIRE(action.action ==
          wh::compose::detail::invoke_runtime::pregel_action::kind::launch);
  REQUIRE(action.node_id == worker_id.value());
  REQUIRE(action.frame.has_value());
  REQUIRE(action.cause.step == 1U);
}

TEST_CASE("pregel loop skips nodes that no longer satisfy current-step readiness",
          "[UT][wh/compose/graph/detail/pregel_loop.hpp][pregel_run_state::take_next_pregel_action][condition][branch][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::pregel, "pregel_loop_skip");
  REQUIRE(graph.has_value());

  wh::core::run_context context{};
  auto base = wh::testing::helper::make_base_run_state(
      graph.value(), wh::compose::graph_value{13}, context);
  wh::compose::detail::invoke_runtime::pregel_run_state pregel_state{
      std::move(base)};
  pregel_state.initialize_pregel_entry();

  auto worker_id = graph->node_id("worker");
  REQUIRE(worker_id.has_value());

  pregel_state.pregel_delivery().clear_current_node(worker_id.value());

  const auto action = pregel_state.take_next_pregel_action(worker_id.value(), 2U);
  REQUIRE(action.action ==
          wh::compose::detail::invoke_runtime::pregel_action::kind::skip);
  REQUIRE(action.node_id == worker_id.value());
  REQUIRE(action.cause.step == 2U);
  REQUIRE(action.cause.node_key == "worker");
}
