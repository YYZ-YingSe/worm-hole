#include <catch2/catch_test_macros.hpp>

#include <tuple>

#include <stdexec/execution.hpp>

#include "helper/compose_graph_runtime_support.hpp"
#include "wh/compose/graph/detail/pregel_checkpoint.hpp"
#include "wh/compose/graph/detail/pregel_checkpoint_inputs.hpp"
#include "wh/compose/graph/detail/start.hpp"

TEST_CASE("pregel pending-input capture emits unit completion for current frontier",
          "[UT][wh/compose/graph/detail/pregel_checkpoint_inputs.hpp][pregel_runtime::capture_pending_inputs][condition][branch][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::pregel, "pregel_pending");
  REQUIRE(graph.has_value());

  wh::core::run_context context{};
  auto base = wh::testing::helper::make_invoke_session(
      graph.value(), wh::compose::graph_value{19}, context);
  wh::compose::detail::invoke_runtime::pregel_runtime pregel_state{
      std::move(base)};
  pregel_state.initialize_entry();

  auto waited = stdexec::sync_wait(pregel_state.capture_pending_inputs());
  REQUIRE(waited.has_value());
  REQUIRE(std::get<0>(*waited).has_value());
  REQUIRE(wh::core::any_cast<std::monostate>(&std::get<0>(*waited).value()) !=
          nullptr);
}

TEST_CASE("pregel pending-input capture becomes a no-op after inputs snapshot already exists",
          "[UT][wh/compose/graph/detail/pregel_checkpoint_inputs.hpp][pregel_runtime::capture_pending_inputs][condition][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::pregel, "pregel_pending_repeat");
  REQUIRE(graph.has_value());

  wh::core::run_context context{};
  auto base = wh::testing::helper::make_invoke_session(
      graph.value(), wh::compose::graph_value{19}, context);
  wh::compose::detail::invoke_runtime::pregel_runtime pregel_state{
      std::move(base)};
  pregel_state.initialize_entry();

  auto first = stdexec::sync_wait(pregel_state.capture_pending_inputs());
  REQUIRE(first.has_value());
  REQUIRE(std::get<0>(*first).has_value());

  auto second = stdexec::sync_wait(pregel_state.capture_pending_inputs());
  REQUIRE(second.has_value());
  REQUIRE(std::get<0>(*second).has_value());
  REQUIRE(wh::core::any_cast<std::monostate>(&std::get<0>(*second).value()) !=
          nullptr);
}

TEST_CASE("pregel runtime checkpoint capture records live deliveries and edge readers",
          "[UT][wh/compose/graph/detail/pregel_checkpoint.hpp][pregel_runtime::capture_checkpoint_runtime][condition][boundary]") {
  auto graph = wh::testing::helper::make_runtime_stream_identity_graph(
      wh::compose::graph_runtime_mode::pregel, "pregel_checkpoint_capture");
  REQUIRE(graph.has_value());

  auto input =
      wh::compose::make_single_value_stream_reader(std::string{"chunk"});
  REQUIRE(input.has_value());

  wh::core::run_context context{};
  auto base = wh::testing::helper::make_invoke_session(
      graph.value(), wh::compose::graph_value{std::move(input).value()}, context);
  wh::compose::detail::invoke_runtime::pregel_runtime pregel_state{
      std::move(base)};
  pregel_state.initialize_entry();

  wh::compose::checkpoint_runtime_state runtime{};
  REQUIRE(pregel_state.capture_checkpoint_runtime(runtime).has_value());
  REQUIRE(runtime.pregel.has_value());
  REQUIRE(runtime.pregel->edge_readers.size() == 1U);
  REQUIRE(runtime.pregel->current_deliveries.size() == 1U);
  REQUIRE(runtime.pregel->current_frontier.size() == 1U);
}
