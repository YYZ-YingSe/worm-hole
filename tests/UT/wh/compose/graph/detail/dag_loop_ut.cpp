#include <catch2/catch_test_macros.hpp>

#include "helper/compose_graph_runtime_support.hpp"
#include "wh/compose/graph/detail/dag_loop.hpp"
#include "wh/compose/graph/detail/start.hpp"

namespace {

class run_state_probe final : public wh::compose::detail::invoke_runtime::run_state {
public:
  using wh::compose::detail::invoke_runtime::run_state::run_state;
  using wh::compose::detail::invoke_runtime::run_state::invoke_state;
  using wh::compose::detail::invoke_runtime::run_state::node_states;
};

} // namespace

TEST_CASE("dag loop selects the next ready launch action from the frontier",
          "[UT][wh/compose/graph/detail/dag_loop.hpp][dag_run_state::take_next_ready_action][condition][branch][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::dag, "dag_loop");
  REQUIRE(graph.has_value());

  wh::core::run_context context{};
  auto base = wh::testing::helper::make_base_run_state(
      graph.value(), wh::compose::graph_value{9}, context);
  wh::compose::detail::invoke_runtime::dag_run_state dag_state{std::move(base)};
  dag_state.initialize_dag_entry();

  auto worker_id = graph->node_id("worker");
  REQUIRE(worker_id.has_value());

  auto action = dag_state.take_next_ready_action();
  REQUIRE(action.kind ==
          wh::compose::detail::invoke_runtime::ready_action_kind::launch);
  REQUIRE(action.frame.has_value());
  REQUIRE(action.frame->node_id == worker_id.value());
  REQUIRE(action.frame->cause.step == 1U);
}

TEST_CASE("dag loop returns continue scan when dequeued work is no longer pending",
          "[UT][wh/compose/graph/detail/dag_loop.hpp][dag_run_state::take_next_ready_action][branch][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::dag, "dag_loop_continue");
  REQUIRE(graph.has_value());

  auto worker_id = graph->node_id("worker");
  REQUIRE(worker_id.has_value());

  wh::core::run_context context{};
  run_state_probe base{
      &graph.value(),
      wh::compose::graph_value{11},
      context,
      wh::compose::graph_call_options{},
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}),
  };
  base.node_states()[worker_id.value()] =
      wh::compose::detail::input_runtime::runtime_node_state::skipped;
  wh::compose::detail::invoke_runtime::dag_run_state dag_state{std::move(base)};
  dag_state.initialize_dag_entry();

  const auto action = dag_state.take_next_ready_action();
  REQUIRE(action.kind ==
          wh::compose::detail::invoke_runtime::ready_action_kind::continue_scan);
  REQUIRE(dag_state.take_next_ready_action().kind ==
          wh::compose::detail::invoke_runtime::ready_action_kind::no_ready);
}

TEST_CASE("dag loop reports terminal timeout when the step budget is exceeded",
          "[UT][wh/compose/graph/detail/dag_loop.hpp][dag_run_state::take_next_ready_action][condition][branch][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::dag, "dag_loop_budget");
  REQUIRE(graph.has_value());

  auto worker_id = graph->node_id("worker");
  REQUIRE(worker_id.has_value());

  wh::core::run_context context{};
  run_state_probe base{
      &graph.value(),
      wh::compose::graph_value{12},
      context,
      wh::compose::graph_call_options{},
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}),
  };
  base.invoke_state().step_budget = 0U;
  wh::compose::detail::invoke_runtime::dag_run_state dag_state{std::move(base)};
  dag_state.initialize_dag_entry();

  const auto action = dag_state.take_next_ready_action();
  REQUIRE(action.kind ==
          wh::compose::detail::invoke_runtime::ready_action_kind::terminal_error);
  REQUIRE(action.error == wh::core::errc::timeout);
}
