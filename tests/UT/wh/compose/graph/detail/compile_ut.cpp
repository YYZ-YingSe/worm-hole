#include <string>

#include <catch2/catch_test_macros.hpp>

#include "helper/compose_graph_test_utils.hpp"
#include "wh/compose/graph.hpp"
#include "wh/compose/graph/detail/compile.hpp"
#include "wh/compose/node.hpp"
#include "wh/compose/node/passthrough.hpp"

TEST_CASE("compile helpers derive gates snapshots and lowering kinds from graph contracts",
          "[UT][wh/compose/graph/detail/"
          "compile.hpp][detail::to_snapshot_edge][condition][branch][boundary]") {
  wh::compose::graph_boundary stream_boundary{
      .input = wh::compose::node_contract::stream,
      .output = wh::compose::node_contract::stream,
  };
  REQUIRE(wh::compose::detail::default_boundary_input_gate(stream_boundary) ==
          wh::compose::input_gate::reader());
  REQUIRE(wh::compose::detail::default_boundary_output_gate(stream_boundary) ==
          wh::compose::output_gate::reader());

  wh::compose::graph_compile_options options{};
  options.name = "compile-ut";
  options.mode = wh::compose::graph_runtime_mode::pregel;
  options.max_steps = 9U;
  options.retry_budget = 2U;
  auto snapshot = wh::compose::detail::to_snapshot_compile_options(options);
  REQUIRE(snapshot.name == "compile-ut");
  REQUIRE(snapshot.mode == wh::compose::graph_runtime_mode::pregel);
  REQUIRE(snapshot.max_steps == 9U);
  REQUIRE(snapshot.retry_budget == 2U);

  wh::compose::edge_adapter auto_adapter{};
  REQUIRE(wh::compose::detail::resolve_edge_lowering_kind(
              wh::compose::node_contract::value, wh::compose::node_contract::stream,
              auto_adapter) == wh::compose::edge_lowering_kind::value_to_stream);
  REQUIRE(wh::compose::detail::resolve_edge_lowering_kind(
              wh::compose::node_contract::stream, wh::compose::node_contract::value,
              auto_adapter) == wh::compose::edge_lowering_kind::stream_to_value);
  auto_adapter.kind = wh::compose::edge_adapter_kind::custom;
  REQUIRE(wh::compose::detail::has_custom_edge_lowering(auto_adapter));

  wh::compose::graph_edge edge{
      .from = "left",
      .to = "right",
      .options =
          {
              .limits = {.max_items = 4U},
          },
  };
  auto snapshot_edge = wh::compose::detail::to_snapshot_edge(
      edge, wh::compose::node_contract::stream, wh::compose::node_contract::value);
  REQUIRE(snapshot_edge.from == "left");
  REQUIRE(snapshot_edge.to == "right");
  REQUIRE(snapshot_edge.lowering_kind == wh::compose::edge_lowering_kind::stream_to_value);
  REQUIRE(snapshot_edge.limits.max_items == 4U);
}

TEST_CASE(
    "compile graph constructors preserve boundary metadata and report cycle diagnostics",
    "[UT][wh/compose/graph/detail/compile.hpp][graph::compile][condition][branch][boundary]") {
  wh::compose::graph_compile_options compiled_options{};
  compiled_options.name = "cycle-ut";

  wh::compose::graph compiled_graph{
      wh::compose::graph_boundary{
          .input = wh::compose::node_contract::stream,
          .output = wh::compose::node_contract::stream,
      },
      compiled_options,
  };
  REQUIRE(compiled_graph.boundary_input_gate() == wh::compose::input_gate::reader());
  REQUIRE(compiled_graph.boundary_output_gate() == wh::compose::output_gate::reader());
  REQUIRE(compiled_graph.add_passthrough(wh::compose::make_passthrough_node("worker")).has_value());
  REQUIRE(compiled_graph.add_entry_edge("worker").has_value());
  REQUIRE(compiled_graph.add_exit_edge("worker").has_value());
  REQUIRE(compiled_graph.compile().has_value());
  REQUIRE(compiled_graph.snapshot_view().nodes.size() == 1U);
  REQUIRE(compiled_graph.compile_options_snapshot().name == "cycle-ut");

  wh::compose::graph cycle_graph{};
  REQUIRE(cycle_graph.add_lambda(wh::testing::helper::make_int_add_node("a", 1)).has_value());
  REQUIRE(cycle_graph.add_lambda(wh::testing::helper::make_int_add_node("b", 2)).has_value());
  REQUIRE(cycle_graph.add_entry_edge("a").has_value());
  REQUIRE(cycle_graph.add_edge("a", "b").has_value());
  REQUIRE(cycle_graph.add_edge("b", "a").has_value());
  REQUIRE(cycle_graph.add_exit_edge("b").has_value());

  auto compiled = cycle_graph.compile();
  REQUIRE(compiled.has_error());
  REQUIRE(compiled.error() == wh::core::errc::contract_violation);
  REQUIRE_FALSE(cycle_graph.diagnostics().empty());
  REQUIRE(cycle_graph.diagnostics().back().message.find("cycle") != std::string::npos);
}
