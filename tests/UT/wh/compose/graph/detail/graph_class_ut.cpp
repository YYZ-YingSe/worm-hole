#include <catch2/catch_test_macros.hpp>

#include <tuple>

#include <stdexec/execution.hpp>

#include "wh/compose/graph/detail/graph_class.hpp"
#include "wh/compose/graph/detail/inline_impl.hpp"
#include "wh/compose/node/passthrough.hpp"

TEST_CASE("graph class declaration remains usable through detail header for construction compile copy and invoke",
          "[UT][wh/compose/graph/detail/graph_class.hpp][graph::graph][condition][branch][boundary]") {
  wh::compose::graph boundary_graph{
      wh::compose::graph_boundary{
          .input = wh::compose::node_contract::value,
          .output = wh::compose::node_contract::stream,
      }};
  REQUIRE(boundary_graph.boundary().output ==
          wh::compose::node_contract::stream);

  wh::compose::graph_compile_options options{};
  options.name = "detail_graph";
  options.max_parallel_nodes = 3U;
  wh::compose::graph graph{options};
  REQUIRE(graph.options().name == "detail_graph");
  REQUIRE(graph.options().max_parallel_nodes == 3U);
  REQUIRE_FALSE(graph.compiled());

  REQUIRE(graph.add_passthrough(wh::compose::make_passthrough_node("worker"))
              .has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());
  REQUIRE(graph.compiled());
  REQUIRE(graph.node_id("worker").has_value());
  REQUIRE(graph.compiled_node_by_key("worker").has_value());
  REQUIRE(graph.snapshot_view().nodes.size() == 1U);
  REQUIRE(graph.restore_shape().nodes.size() == 1U);

  wh::compose::graph copied{graph};
  REQUIRE(copied.compiled());
  REQUIRE(copied.node_id("worker").has_value());

  wh::compose::graph moved{std::move(copied)};
  REQUIRE(moved.compiled());
  REQUIRE(moved.node_id("worker").has_value());

  wh::compose::graph assigned{};
  assigned = moved;
  REQUIRE(assigned.compiled());
  REQUIRE(assigned.node_id("worker").has_value());

  wh::compose::graph_invoke_request request{};
  request.input = wh::compose::graph_input::value(42);
  wh::core::run_context context{};
  auto waited = stdexec::sync_wait(assigned.invoke(context, std::move(request)));
  REQUIRE(waited.has_value());
  REQUIRE(std::get<0>(*waited).has_value());
  REQUIRE(std::get<0>(*waited).value().output_status.has_value());
  REQUIRE(*wh::core::any_cast<int>(
              &std::get<0>(*waited).value().output_status.value()) == 42);
}

TEST_CASE("graph class reports missing nodes and rejects compiled-node lookup before compile",
          "[UT][wh/compose/graph/detail/graph_class.hpp][graph::compiled_node_by_key][condition][branch][boundary]") {
  wh::compose::graph graph{};
  REQUIRE(graph.add_passthrough("worker").has_value());

  REQUIRE(graph.node_id("worker").has_value());
  auto missing_id = graph.node_id("missing");
  REQUIRE(missing_id.has_error());
  REQUIRE(missing_id.error() == wh::core::errc::not_found);

  auto before_compile = graph.compiled_node_by_key("worker");
  REQUIRE(before_compile.has_error());
  REQUIRE(before_compile.error() == wh::core::errc::contract_violation);

  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  auto missing_compiled = graph.compiled_node_by_key("missing");
  REQUIRE(missing_compiled.has_error());
  REQUIRE(missing_compiled.error() == wh::core::errc::not_found);

  auto second_compile = graph.compile();
  REQUIRE(second_compile.has_error());
  REQUIRE(second_compile.error() == wh::core::errc::contract_violation);
  REQUIRE_FALSE(graph.diagnostics().empty());
}
