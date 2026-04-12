#include <catch2/catch_test_macros.hpp>

#include <tuple>

#include <stdexec/execution.hpp>

#include "helper/compose_graph_runtime_support.hpp"
#include "wh/compose/graph/detail/invoke.hpp"
#include "wh/compose/node/lambda.hpp"

TEST_CASE("graph invoke report promotes node run error into graph run error",
          "[UT][wh/compose/graph/detail/invoke.hpp][make_graph_run_report][condition][branch]") {
  wh::compose::detail::runtime_state::invoke_outputs outputs{};
  outputs.node_run_error = wh::compose::graph_node_run_error_detail{
      .path = wh::compose::make_node_path({"graph", "worker"}),
      .node = "worker",
      .code = wh::core::errc::internal_error,
      .raw_error = wh::core::errc::invalid_argument,
      .message = "node-failed",
  };

  const auto status = wh::core::result<wh::compose::graph_value>::failure(
      wh::core::errc::internal_error);
  auto report =
      wh::compose::detail::make_graph_run_report(status, std::move(outputs));

  REQUIRE(report.graph_run_error.has_value());
  REQUIRE(report.graph_run_error->phase ==
          wh::compose::compose_error_phase::execute);
  REQUIRE(report.graph_run_error->path ==
          std::optional{wh::compose::make_node_path({"graph", "worker"})});
  REQUIRE(report.graph_run_error->node == "worker");
  REQUIRE(report.graph_run_error->code == wh::core::errc::internal_error);
  REQUIRE(report.graph_run_error->raw_error ==
          std::optional{wh::core::error_code{wh::core::errc::invalid_argument}});
  REQUIRE(report.graph_run_error->message == "node-failed");
}

TEST_CASE("graph invoke report promotes stream read error when node run error is absent",
          "[UT][wh/compose/graph/detail/invoke.hpp][make_graph_run_report][branch][boundary]") {
  wh::compose::detail::runtime_state::invoke_outputs outputs{};
  outputs.stream_read_error = wh::compose::graph_new_stream_read_error_detail{
      .path = wh::compose::make_node_path({"graph", "stream"}),
      .node = "stream",
      .code = wh::core::errc::invalid_argument,
      .raw_error = wh::core::errc::protocol_error,
      .message = "stream-read-failed",
  };

  const auto status = wh::core::result<wh::compose::graph_value>::failure(
      wh::core::errc::invalid_argument);
  auto report =
      wh::compose::detail::make_graph_run_report(status, std::move(outputs));

  REQUIRE(report.graph_run_error.has_value());
  REQUIRE(report.graph_run_error->phase ==
          wh::compose::compose_error_phase::execute);
  REQUIRE(report.graph_run_error->path ==
          std::optional{wh::compose::make_node_path({"graph", "stream"})});
  REQUIRE(report.graph_run_error->node == "stream");
  REQUIRE(report.graph_run_error->code == wh::core::errc::invalid_argument);
  REQUIRE(report.graph_run_error->raw_error ==
          std::optional{wh::core::error_code{wh::core::errc::protocol_error}});
  REQUIRE(report.graph_run_error->message == "stream-read-failed");
}

TEST_CASE("graph invoke report keeps existing graph run error detail intact",
          "[UT][wh/compose/graph/detail/invoke.hpp][make_graph_run_report][condition][boundary]") {
  wh::compose::detail::runtime_state::invoke_outputs outputs{};
  outputs.graph_run_error = wh::compose::graph_run_error_detail{
      .phase = wh::compose::compose_error_phase::checkpoint,
      .path = wh::compose::make_node_path({"graph", "checkpoint"}),
      .node = "checkpoint",
      .code = wh::core::errc::config_error,
      .raw_error = wh::core::error_code{wh::core::errc::parse_error},
      .message = "existing-graph-error",
  };
  outputs.node_run_error = wh::compose::graph_node_run_error_detail{
      .path = wh::compose::make_node_path({"graph", "worker"}),
      .node = "worker",
      .code = wh::core::errc::internal_error,
      .raw_error = wh::core::errc::invalid_argument,
      .message = "node-failed",
  };

  const auto status = wh::core::result<wh::compose::graph_value>::failure(
      wh::core::errc::config_error);
  auto report =
      wh::compose::detail::make_graph_run_report(status, std::move(outputs));

  REQUIRE(report.graph_run_error.has_value());
  REQUIRE(report.graph_run_error->phase ==
          wh::compose::compose_error_phase::checkpoint);
  REQUIRE(report.graph_run_error->path ==
          std::optional{wh::compose::make_node_path({"graph", "checkpoint"})});
  REQUIRE(report.graph_run_error->node == "checkpoint");
  REQUIRE(report.graph_run_error->code == wh::core::errc::config_error);
  REQUIRE(report.graph_run_error->raw_error ==
          std::optional{wh::core::error_code{wh::core::errc::parse_error}});
  REQUIRE(report.graph_run_error->message == "existing-graph-error");
}

TEST_CASE("graph invoke sender returns output status and report through public completion channel",
          "[UT][wh/compose/graph/detail/invoke.hpp][graph::invoke][condition][branch][boundary]") {
  wh::compose::graph graph{};
  REQUIRE(graph
              .add_lambda(wh::compose::make_lambda_node(
                  "worker",
                  [](wh::compose::graph_value &, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    return wh::core::result<wh::compose::graph_value>::failure(
                        wh::core::errc::invalid_argument);
                  }))
              .has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  wh::compose::graph_invoke_request request{};
  request.input = wh::compose::graph_value{1};
  wh::core::run_context context{};
  auto waited = stdexec::sync_wait(graph.invoke(context, std::move(request)));
  REQUIRE(waited.has_value());

  auto invoke_status = std::get<0>(std::move(*waited));
  REQUIRE(invoke_status.has_value());
  REQUIRE(invoke_status->output_status.has_error());
  REQUIRE(invoke_status->output_status.error() ==
          wh::core::errc::invalid_argument);
  REQUIRE(invoke_status->report.node_run_error.has_value());
  REQUIRE(invoke_status->report.node_run_error->node == "worker");
  REQUIRE(invoke_status->report.graph_run_error.has_value());
  REQUIRE(invoke_status->report.graph_run_error->node == "worker");
}

TEST_CASE("start_bound_graph and start_scoped_graph run nested graph entrypoints with bound scope data",
          "[UT][wh/compose/graph/detail/invoke.hpp][start_bound_graph][condition][branch]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::dag, "invoke_bound");
  REQUIRE(graph.has_value());

  wh::core::run_context context{};
  wh::compose::graph_value input{42};
  wh::compose::graph_call_options call_options{};
  const auto scheduler =
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{});

  auto waited = stdexec::sync_wait(wh::compose::detail::start_bound_graph(
      graph.value(), context, input, &call_options, nullptr, nullptr, nullptr,
      scheduler, nullptr, nullptr, {}));
  REQUIRE(waited.has_value());
  REQUIRE(std::get<0>(*waited).has_value());
  REQUIRE(*wh::core::any_cast<int>(&std::get<0>(*waited).value()) == 42);

  wh::compose::graph_value second_input{99};
  wh::compose::graph_call_scope scope{call_options};
  auto scoped_waited = stdexec::sync_wait(wh::compose::detail::start_scoped_graph(
      graph.value(), context, second_input, &scope, nullptr, nullptr, nullptr,
      scheduler, nullptr, nullptr, {}));
  REQUIRE(scoped_waited.has_value());
  REQUIRE(std::get<0>(*scoped_waited).has_value());
  REQUIRE(*wh::core::any_cast<int>(&std::get<0>(*scoped_waited).value()) == 99);
}
