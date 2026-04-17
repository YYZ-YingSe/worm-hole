#include <catch2/catch_test_macros.hpp>

#include <tuple>

#include <stdexec/execution.hpp>

#include "helper/compose_graph_runtime_support.hpp"
#include "wh/compose/graph/detail/pregel.hpp"
#include "wh/compose/graph/detail/start.hpp"

TEST_CASE("pregel runtime wrapper builds node input sender from current delivery frontier",
          "[UT][wh/compose/graph/detail/pregel.hpp][pregel_runtime::make_input_sender][condition][branch][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::pregel, "pregel_wrapper");
  REQUIRE(graph.has_value());

  wh::core::run_context context{};
  auto base = wh::testing::helper::make_invoke_session(
      graph.value(), wh::compose::graph_value{21}, context);
  wh::compose::detail::invoke_runtime::pregel_runtime pregel_state{
      std::move(base)};
  pregel_state.initialize_entry();

  auto worker_id = graph->node_id("worker");
  REQUIRE(worker_id.has_value());
  auto action = pregel_state.take_ready_action(worker_id.value(), 1U);
  REQUIRE(action.action ==
          wh::compose::detail::invoke_runtime::pregel_action::kind::launch);
  REQUIRE(action.frame.has_value());

  auto waited =
      stdexec::sync_wait(pregel_state.make_input_sender(&action.frame.value()));
  REQUIRE(waited.has_value());
  REQUIRE(std::get<0>(*waited).has_value());
  REQUIRE(*wh::core::any_cast<int>(&std::get<0>(*waited).value()) == 21);
}

TEST_CASE("pregel runtime wrapper reports not_found when asked to build input for an unknown node",
          "[UT][wh/compose/graph/detail/pregel.hpp][pregel_runtime::make_input_sender][branch][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::pregel, "pregel_wrapper_invalid");
  REQUIRE(graph.has_value());

  wh::core::run_context context{};
  auto base = wh::testing::helper::make_invoke_session(
      graph.value(), wh::compose::graph_value{21}, context);
  wh::compose::detail::invoke_runtime::pregel_runtime pregel_state{
      std::move(base)};

  wh::compose::detail::invoke_runtime::node_frame frame{};
  frame.node_id = 999U;

  auto waited = stdexec::sync_wait(pregel_state.make_input_sender(&frame));
  REQUIRE(waited.has_value());
  REQUIRE(std::get<0>(*waited).has_error());
  REQUIRE(std::get<0>(*waited).error() == wh::core::errc::not_found);
}
