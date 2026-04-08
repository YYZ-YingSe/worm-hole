#include <catch2/catch_test_macros.hpp>

#include <tuple>

#include <stdexec/execution.hpp>

#include "helper/compose_graph_runtime_support.hpp"
#include "wh/compose/graph/detail/dag_pending_input.hpp"
#include "wh/compose/graph/detail/start.hpp"

TEST_CASE("dag pending-input capture emits unit completion for pending launch set",
          "[UT][wh/compose/graph/detail/dag_pending_input.hpp][dag_run_state::capture_dag_pending_inputs][condition][branch][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::dag, "dag_pending");
  REQUIRE(graph.has_value());

  wh::core::run_context context{};
  auto base = wh::testing::helper::make_base_run_state(
      graph.value(), wh::compose::graph_value{17}, context);
  wh::compose::detail::invoke_runtime::dag_run_state dag_state{std::move(base)};
  dag_state.initialize_dag_entry();

  auto waited = stdexec::sync_wait(dag_state.capture_dag_pending_inputs());
  REQUIRE(waited.has_value());
  REQUIRE(std::get<0>(*waited).has_value());
  REQUIRE(wh::core::any_cast<std::monostate>(&std::get<0>(*waited).value()) !=
          nullptr);
}

TEST_CASE("dag pending-input capture becomes a no-op after rerun snapshot already exists",
          "[UT][wh/compose/graph/detail/dag_pending_input.hpp][dag_run_state::capture_dag_pending_inputs][condition][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::dag, "dag_pending_repeat");
  REQUIRE(graph.has_value());

  wh::core::run_context context{};
  auto base = wh::testing::helper::make_base_run_state(
      graph.value(), wh::compose::graph_value{17}, context);
  wh::compose::detail::invoke_runtime::dag_run_state dag_state{std::move(base)};
  dag_state.initialize_dag_entry();

  auto first = stdexec::sync_wait(dag_state.capture_dag_pending_inputs());
  REQUIRE(first.has_value());
  REQUIRE(std::get<0>(*first).has_value());

  auto second = stdexec::sync_wait(dag_state.capture_dag_pending_inputs());
  REQUIRE(second.has_value());
  REQUIRE(std::get<0>(*second).has_value());
  REQUIRE(wh::core::any_cast<std::monostate>(&std::get<0>(*second).value()) !=
          nullptr);
}
