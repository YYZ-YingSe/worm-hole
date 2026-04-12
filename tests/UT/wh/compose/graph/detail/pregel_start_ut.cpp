#include <catch2/catch_test_macros.hpp>

#include <tuple>

#include <stdexec/execution.hpp>

#include "helper/compose_graph_runtime_support.hpp"
#include "wh/compose/graph/detail/pregel_start.hpp"

TEST_CASE("pregel run sender connects initialized pregel runtime state into a working sender surface",
          "[UT][wh/compose/graph/detail/pregel_start.hpp][pregel_run_sender::connect][condition][branch][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::pregel, "pregel_sender");
  REQUIRE(graph.has_value());

  wh::core::run_context context{};
  auto base = wh::testing::helper::make_base_run_state(
      graph.value(), wh::compose::graph_value{9}, context);
  wh::compose::detail::invoke_runtime::pregel_run_state pregel_state{
      std::move(base)};
  pregel_state.initialize_pregel_entry();

  auto waited = stdexec::sync_wait(
      wh::compose::detail::invoke_runtime::pregel_run_sender{
          std::move(pregel_state)});
  REQUIRE(waited.has_value());
  REQUIRE(std::get<0>(*waited).has_value());
  auto *typed = wh::core::any_cast<int>(&std::get<0>(*waited).value());
  REQUIRE(typed != nullptr);
  REQUIRE(*typed == 9);
}

TEST_CASE("pregel run sender preserves owned call options when rebinding moved runtime storage",
          "[UT][wh/compose/graph/detail/pregel_start.hpp][pregel_run_sender::connect][condition][branch]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::pregel, "pregel_sender_trace");
  REQUIRE(graph.has_value());

  wh::compose::graph_call_options call_options{};
  call_options.trace = wh::compose::graph_trace_context{
      .trace_id = "pregel-trace",
      .parent_span_id = "root",
  };

  wh::core::run_context context{};
  wh::compose::detail::invoke_runtime::run_state base{
      &graph.value(),
      wh::compose::graph_value{19},
      context,
      std::move(call_options),
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}),
  };
  wh::compose::detail::invoke_runtime::pregel_run_state pregel_state{
      std::move(base)};
  pregel_state.initialize_pregel_entry();

  auto waited = stdexec::sync_wait(
      wh::compose::detail::invoke_runtime::pregel_run_sender{
          std::move(pregel_state)});
  REQUIRE(waited.has_value());
  REQUIRE(std::get<0>(*waited).has_value());
  REQUIRE(*wh::core::any_cast<int>(&std::get<0>(*waited).value()) == 19);
}
