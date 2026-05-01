#include <algorithm>
#include <tuple>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "wh/compose/graph.hpp"
#include "wh/compose/graph/detail/control.hpp"
#include "wh/compose/graph/detail/inline_impl.hpp"
#include "wh/compose/node.hpp"
#include "wh/compose/node/lambda.hpp"

namespace {

[[nodiscard]] auto make_control_graph() -> wh::compose::graph {
  wh::compose::graph graph{};
  auto added = graph.add_lambda(wh::compose::make_lambda_node(
      "worker",
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
        return input;
      }));
  REQUIRE(added.has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());
  return graph;
}

} // namespace

TEST_CASE("control helpers reject invalid call options through public invoke validation",
          "[UT][wh/compose/graph/detail/"
          "control.hpp][graph::validate_call_scope_for_runtime][condition][branch][boundary]") {
  wh::compose::graph graph = make_control_graph();
  wh::core::run_context context{};

  SECTION("invalid component default key") {
    wh::compose::graph_invoke_request request{};
    request.input = wh::compose::graph_input::value(1);
    request.controls.call.component_defaults.emplace("bad/key", wh::compose::graph_value{7});
    auto waited = stdexec::sync_wait(graph.invoke(context, std::move(request)));
    REQUIRE(waited.has_value());
    REQUIRE(std::get<0>(*waited).has_value());
    REQUIRE(std::get<0>(*waited)->output_status.has_error());
    REQUIRE(std::get<0>(*waited)->output_status.error() == wh::core::errc::invalid_argument);
    REQUIRE(std::get<0>(*waited)->report.graph_run_error.has_value());
  }

  SECTION("missing designated top-level node") {
    wh::compose::graph_invoke_request request{};
    request.input = wh::compose::graph_input::value(1);
    request.controls.call.designated_top_level_nodes = {"missing"};
    auto waited = stdexec::sync_wait(graph.invoke(context, std::move(request)));
    REQUIRE(waited.has_value());
    REQUIRE(std::get<0>(*waited).has_value());
    REQUIRE(std::get<0>(*waited)->output_status.has_error());
    REQUIRE(std::get<0>(*waited)->output_status.error() == wh::core::errc::not_found);
    REQUIRE(std::get<0>(*waited)->report.graph_run_error.has_value());
  }

  SECTION("null debug observer callback") {
    wh::compose::graph_invoke_request request{};
    request.input = wh::compose::graph_input::value(1);
    request.controls.call.node_path_debug_observers.push_back(
        {.path = wh::compose::make_node_path({"worker"}),
         .include_descendants = true,
         .callback = nullptr});
    auto waited = stdexec::sync_wait(graph.invoke(context, std::move(request)));
    REQUIRE(waited.has_value());
    REQUIRE(std::get<0>(*waited).has_value());
    REQUIRE(std::get<0>(*waited)->output_status.has_error());
    REQUIRE(std::get<0>(*waited)->output_status.error() == wh::core::errc::invalid_argument);
    REQUIRE(std::get<0>(*waited)->report.graph_run_error.has_value());
  }
}

TEST_CASE("control helpers respect valid designation paths through public invoke",
          "[UT][wh/compose/graph/detail/control.hpp][graph::is_node_designated][branch]") {
  wh::compose::graph graph = make_control_graph();
  wh::compose::graph_invoke_request request{};
  request.input = wh::compose::graph_input::value(9);
  request.controls.call.designated_top_level_nodes = {"worker"};
  request.controls.call.designated_node_paths = {wh::compose::make_node_path({"worker"})};
  wh::core::run_context context{};
  auto waited = stdexec::sync_wait(graph.invoke(context, std::move(request)));
  REQUIRE(waited.has_value());
  REQUIRE(std::get<0>(*waited).has_value());
  REQUIRE(std::get<0>(*waited)->output_status.has_value());
  REQUIRE(*wh::core::any_cast<int>(&std::get<0>(*waited)->output_status.value()) == 9);
}
