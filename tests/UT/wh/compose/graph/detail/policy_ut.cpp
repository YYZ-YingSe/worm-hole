#include <tuple>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "helper/compose_graph_runtime_support.hpp"
#include "wh/compose/graph/detail/policy.hpp"

TEST_CASE("policy helpers enforce step-budget rules across dag and pregel invoke paths",
          "[UT][wh/compose/graph/detail/"
          "policy.hpp][graph::resolve_step_budget][condition][branch][boundary]") {
  auto dag_graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::dag, "policy_dag");
  REQUIRE(dag_graph.has_value());

  wh::compose::graph_invoke_request dag_request{};
  dag_request.input = wh::compose::graph_input::value(3);
  dag_request.controls.call.pregel_max_steps = 2U;
  wh::core::run_context dag_context{};
  auto dag_waited = stdexec::sync_wait(dag_graph->invoke(dag_context, std::move(dag_request)));
  REQUIRE(dag_waited.has_value());
  REQUIRE(std::get<0>(*dag_waited).has_value());
  REQUIRE(std::get<0>(*dag_waited)->output_status.has_error());
  REQUIRE(std::get<0>(*dag_waited)->output_status.error() == wh::core::errc::contract_violation);
  REQUIRE(std::get<0>(*dag_waited)->report.graph_run_error.has_value());

  auto pregel_graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::pregel, "policy_pregel");
  REQUIRE(pregel_graph.has_value());

  wh::compose::graph_invoke_request zero_step_request{};
  zero_step_request.input = wh::compose::graph_input::value(4);
  zero_step_request.controls.call.pregel_max_steps = 0U;
  wh::core::run_context zero_step_context{};
  auto zero_step_waited =
      stdexec::sync_wait(pregel_graph->invoke(zero_step_context, std::move(zero_step_request)));
  REQUIRE(zero_step_waited.has_value());
  REQUIRE(std::get<0>(*zero_step_waited).has_value());
  REQUIRE(std::get<0>(*zero_step_waited)->output_status.has_error());
  REQUIRE(std::get<0>(*zero_step_waited)->output_status.error() ==
          wh::core::errc::invalid_argument);
  REQUIRE(std::get<0>(*zero_step_waited)->report.graph_run_error.has_value());

  wh::compose::graph_invoke_request pregel_request{};
  pregel_request.input = wh::compose::graph_input::value(5);
  pregel_request.controls.call.pregel_max_steps = 2U;
  wh::core::run_context pregel_context{};
  auto pregel_waited =
      stdexec::sync_wait(pregel_graph->invoke(pregel_context, std::move(pregel_request)));
  REQUIRE(pregel_waited.has_value());
  REQUIRE(std::get<0>(*pregel_waited).has_value());
  REQUIRE(std::get<0>(*pregel_waited)->output_status.has_value());
  REQUIRE(*wh::core::any_cast<int>(&std::get<0>(*pregel_waited)->output_status.value()) == 5);
}

TEST_CASE(
    "policy helpers reject invalid compile-time pregel budgets when no per-call override is "
    "provided",
    "[UT][wh/compose/graph/detail/policy.hpp][graph::resolve_step_budget][condition][branch]") {
  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::pregel;
  options.name = "policy_invalid_default";
  options.max_steps = 0U;

  wh::compose::graph bad_graph{std::move(options)};
  REQUIRE(bad_graph
              .add_lambda(wh::compose::make_lambda_node(
                  "worker",
                  [](wh::compose::graph_value &value, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> { return std::move(value); }))
              .has_value());
  REQUIRE(bad_graph.add_entry_edge("worker").has_value());
  REQUIRE(bad_graph.add_exit_edge("worker").has_value());
  auto compiled = bad_graph.compile();
  REQUIRE(compiled.has_error());
  REQUIRE(compiled.error() == wh::core::errc::invalid_argument);
}
