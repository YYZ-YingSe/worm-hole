#include <catch2/catch_test_macros.hpp>

#include <tuple>

#include <stdexec/execution.hpp>

#include "helper/compose_graph_runtime_support.hpp"
#include "wh/compose/graph/detail/dag.hpp"
#include "wh/compose/graph/detail/start.hpp"

TEST_CASE("dag runtime wrapper builds node input sender from pending frontier frame",
          "[UT][wh/compose/graph/detail/dag.hpp][dag_runtime::make_input_sender][condition][branch][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::dag, "dag_wrapper");
  REQUIRE(graph.has_value());

  wh::core::run_context context{};
  auto base = wh::testing::helper::make_invoke_session(
      graph.value(), wh::compose::graph_value{13}, context);
  wh::compose::detail::invoke_runtime::dag_runtime dag_state{std::move(base)};
  dag_state.initialize_entry();

  auto worker_id = graph->node_id("worker");
  REQUIRE(worker_id.has_value());
  auto action = dag_state.take_ready_action();
  REQUIRE(action.kind ==
          wh::compose::detail::invoke_runtime::ready_action_kind::launch);
  REQUIRE(action.attempt.has_value());
  REQUIRE(action.attempt.slot == worker_id.value());

  auto waited = stdexec::sync_wait(dag_state.make_input_sender(action.attempt));
  REQUIRE(waited.has_value());
  REQUIRE(std::get<0>(*waited).has_value());
  REQUIRE(*wh::core::any_cast<int>(&std::get<0>(*waited).value()) == 13);
}
