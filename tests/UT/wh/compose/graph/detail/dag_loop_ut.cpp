#include <catch2/catch_test_macros.hpp>

#include "helper/compose_graph_runtime_support.hpp"
#include "wh/compose/graph/detail/dag_loop.hpp"
#include "wh/compose/graph/detail/start.hpp"

namespace {

class invoke_session_probe final
    : public wh::compose::detail::invoke_runtime::invoke_session {
public:
  using wh::compose::detail::invoke_runtime::invoke_session::invoke_session;
  using wh::compose::detail::invoke_runtime::invoke_session::invoke_state;
};

} // namespace

TEST_CASE("dag loop selects the next ready launch action from the frontier",
          "[UT][wh/compose/graph/detail/dag_loop.hpp][dag_runtime::take_ready_action][condition][branch][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::dag, "dag_loop");
  REQUIRE(graph.has_value());

  wh::core::run_context context{};
  auto base = wh::testing::helper::make_invoke_session(
      graph.value(), wh::compose::graph_value{9}, context);
  wh::compose::detail::invoke_runtime::dag_runtime dag_state{std::move(base)};
  dag_state.initialize_entry();

  auto worker_id = graph->node_id("worker");
  REQUIRE(worker_id.has_value());

  auto action = dag_state.take_ready_action();
  REQUIRE(action.kind ==
          wh::compose::detail::invoke_runtime::ready_action_kind::launch);
  REQUIRE(action.attempt.has_value());
  REQUIRE(action.attempt.slot == worker_id.value());
}

TEST_CASE("dag loop returns continue scan when dequeued work is no longer pending",
          "[UT][wh/compose/graph/detail/dag_loop.hpp][dag_runtime::take_ready_action][branch][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::dag, "dag_loop_continue");
  REQUIRE(graph.has_value());

  auto worker_id = graph->node_id("worker");
  REQUIRE(worker_id.has_value());

  wh::core::run_context context{};
  invoke_session_probe base{
      &graph.value(),
      wh::compose::graph_value{11},
      context,
      wh::compose::graph_call_options{},
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}),
  };
  wh::compose::detail::invoke_runtime::dag_runtime dag_state{std::move(base)};
  dag_state.initialize_entry();
  dag_state.dag_node_phases()[worker_id.value()] =
      wh::compose::detail::input_runtime::dag_node_phase::skipped;

  const auto action = dag_state.take_ready_action();
  REQUIRE(action.kind ==
          wh::compose::detail::invoke_runtime::ready_action_kind::continue_scan);
  REQUIRE(dag_state.take_ready_action().kind ==
          wh::compose::detail::invoke_runtime::ready_action_kind::no_ready);
}

TEST_CASE("dag loop reports terminal timeout when the step budget is exceeded",
          "[UT][wh/compose/graph/detail/dag_loop.hpp][dag_runtime::take_ready_action][condition][branch][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::dag, "dag_loop_budget");
  REQUIRE(graph.has_value());

  auto worker_id = graph->node_id("worker");
  REQUIRE(worker_id.has_value());

  wh::core::run_context context{};
  invoke_session_probe base{
      &graph.value(),
      wh::compose::graph_value{12},
      context,
      wh::compose::graph_call_options{},
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}),
  };
  base.invoke_state().step_budget = 0U;
  wh::compose::detail::invoke_runtime::dag_runtime dag_state{std::move(base)};
  dag_state.initialize_entry();

  const auto action = dag_state.take_ready_action();
  REQUIRE(action.kind ==
          wh::compose::detail::invoke_runtime::ready_action_kind::terminal_error);
  REQUIRE(action.error == wh::core::errc::timeout);
}
