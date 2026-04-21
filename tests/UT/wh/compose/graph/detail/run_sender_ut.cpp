#include <tuple>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "helper/compose_graph_runtime_support.hpp"
#include "wh/compose/graph/detail/run_sender.hpp"

TEST_CASE(
    "graph run sender aliases connect initialized concrete runtimes into a working sender surface",
    "[UT][wh/compose/graph/detail/"
    "run_sender.hpp][dag_run_sender][pregel_run_sender][condition][branch][boundary]") {
  auto dag_graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::dag, "dag_sender");
  REQUIRE(dag_graph.has_value());

  wh::core::run_context dag_context{};
  auto dag_session = wh::testing::helper::make_invoke_session(
      dag_graph.value(), wh::compose::graph_value{8}, dag_context);
  wh::compose::detail::invoke_runtime::dag_runtime dag_state{std::move(dag_session)};
  dag_state.initialize_entry();

  auto dag_waited =
      stdexec::sync_wait(wh::compose::detail::invoke_runtime::dag_run_sender{std::move(dag_state)});
  REQUIRE(dag_waited.has_value());
  REQUIRE(std::get<0>(*dag_waited).has_value());
  auto *dag_value = wh::core::any_cast<int>(&std::get<0>(*dag_waited).value());
  REQUIRE(dag_value != nullptr);
  REQUIRE(*dag_value == 8);

  auto pregel_graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::pregel, "pregel_sender");
  REQUIRE(pregel_graph.has_value());

  wh::core::run_context pregel_context{};
  auto pregel_session = wh::testing::helper::make_invoke_session(
      pregel_graph.value(), wh::compose::graph_value{9}, pregel_context);
  wh::compose::detail::invoke_runtime::pregel_runtime pregel_state{std::move(pregel_session)};
  pregel_state.initialize_entry();

  auto pregel_waited = stdexec::sync_wait(
      wh::compose::detail::invoke_runtime::pregel_run_sender{std::move(pregel_state)});
  REQUIRE(pregel_waited.has_value());
  REQUIRE(std::get<0>(*pregel_waited).has_value());
  auto *pregel_value = wh::core::any_cast<int>(&std::get<0>(*pregel_waited).value());
  REQUIRE(pregel_value != nullptr);
  REQUIRE(*pregel_value == 9);
}

TEST_CASE(
    "graph run sender aliases preserve owned call options when rebinding moved runtime storage",
    "[UT][wh/compose/graph/detail/"
    "run_sender.hpp][dag_run_sender][pregel_run_sender][condition][branch]") {
  wh::compose::graph_call_options dag_call_options{};
  dag_call_options.trace = wh::compose::graph_trace_context{
      .trace_id = "dag-trace",
      .parent_span_id = "root",
  };

  auto dag_graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::dag, "dag_sender_trace");
  REQUIRE(dag_graph.has_value());

  wh::core::run_context dag_context{};
  wh::compose::detail::invoke_runtime::invoke_session dag_session{
      &dag_graph.value(),
      wh::compose::graph_value{18},
      dag_context,
      std::move(dag_call_options),
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}),
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}),
  };
  wh::compose::detail::invoke_runtime::dag_runtime dag_state{std::move(dag_session)};
  dag_state.initialize_entry();

  auto dag_waited =
      stdexec::sync_wait(wh::compose::detail::invoke_runtime::dag_run_sender{std::move(dag_state)});
  REQUIRE(dag_waited.has_value());
  REQUIRE(std::get<0>(*dag_waited).has_value());
  REQUIRE(*wh::core::any_cast<int>(&std::get<0>(*dag_waited).value()) == 18);

  wh::compose::graph_call_options pregel_call_options{};
  pregel_call_options.trace = wh::compose::graph_trace_context{
      .trace_id = "pregel-trace",
      .parent_span_id = "root",
  };

  auto pregel_graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::pregel, "pregel_sender_trace");
  REQUIRE(pregel_graph.has_value());

  wh::core::run_context pregel_context{};
  wh::compose::detail::invoke_runtime::invoke_session pregel_session{
      &pregel_graph.value(),
      wh::compose::graph_value{19},
      pregel_context,
      std::move(pregel_call_options),
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}),
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}),
  };
  wh::compose::detail::invoke_runtime::pregel_runtime pregel_state{std::move(pregel_session)};
  pregel_state.initialize_entry();

  auto pregel_waited = stdexec::sync_wait(
      wh::compose::detail::invoke_runtime::pregel_run_sender{std::move(pregel_state)});
  REQUIRE(pregel_waited.has_value());
  REQUIRE(std::get<0>(*pregel_waited).has_value());
  REQUIRE(*wh::core::any_cast<int>(&std::get<0>(*pregel_waited).value()) == 19);
}
