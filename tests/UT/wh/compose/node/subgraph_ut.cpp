#include <catch2/catch_test_macros.hpp>

#include <tuple>

#include <stdexec/execution.hpp>

#include "wh/compose/graph/graph.hpp"
#include "wh/compose/node/subgraph.hpp"
#include "wh/compose/node/passthrough.hpp"

namespace {

[[nodiscard]] auto make_child_graph() -> wh::compose::graph {
  wh::compose::graph graph{};
  auto added = graph.add_passthrough(wh::compose::make_passthrough_node("inner"));
  REQUIRE(added.has_value());
  REQUIRE(graph.add_entry_edge("inner").has_value());
  REQUIRE(graph.add_exit_edge("inner").has_value());
  return graph;
}

} // namespace

TEST_CASE("subgraph node factories project child boundary metadata into authored descriptors",
          "[UT][wh/compose/node/subgraph.hpp][make_subgraph_node][condition][branch][boundary]") {
  auto child = make_child_graph();
  auto node = wh::compose::make_subgraph_node("child", child);

  REQUIRE(node.key() == "child");
  REQUIRE(node.input_contract() == wh::compose::node_contract::value);
  REQUIRE(node.output_contract() == wh::compose::node_contract::value);
  REQUIRE(node.input_gate() == child.boundary_input_gate());
  REQUIRE(node.output_gate() == child.boundary_output_gate());
  REQUIRE(node.exec_mode() == wh::compose::node_exec_mode::async);
  REQUIRE(node.exec_origin() == wh::compose::node_exec_origin::lowered);
  REQUIRE(node.options().type == "subgraph");
  REQUIRE(node.options().label == "subgraph");
}

TEST_CASE("subgraph nodes execute nested graphs and preserve compiled child snapshots",
          "[UT][wh/compose/node/subgraph.hpp][subgraph_node::compile][condition][branch]") {
  auto child = make_child_graph();
  REQUIRE(child.compile().has_value());

  wh::compose::graph parent{};
  REQUIRE(parent.add_subgraph(wh::compose::make_subgraph_node("child", child))
              .has_value());
  REQUIRE(parent.add_entry_edge("child").has_value());
  REQUIRE(parent.add_exit_edge("child").has_value());
  REQUIRE(parent.compile().has_value());

  auto compiled = parent.compiled_node_by_key("child");
  REQUIRE(compiled.has_value());
  REQUIRE(compiled.value().get().meta.subgraph_snapshot.has_value());
  REQUIRE(compiled.value().get().meta.subgraph_restore_shape.has_value());
  REQUIRE(compiled.value().get().meta.compiled_input_gate ==
          child.boundary_input_gate());
  REQUIRE(compiled.value().get().meta.compiled_output_gate ==
          child.boundary_output_gate());

  wh::compose::graph_invoke_request request{};
  request.input = wh::compose::graph_input::value(23);
  wh::core::run_context context{};
  auto awaited = stdexec::sync_wait(parent.invoke(context, std::move(request)));
  REQUIRE(awaited.has_value());
  REQUIRE(std::get<0>(*awaited).has_value());
  REQUIRE(std::get<0>(*awaited).value().output_status.has_value());
  REQUIRE(*wh::core::any_cast<int>(
              &std::get<0>(*awaited).value().output_status.value()) == 23);
}

TEST_CASE("subgraph nodes also auto-compile owned child graphs during parent compile",
          "[UT][wh/compose/node/subgraph.hpp][subgraph_node::compile][boundary]") {
  auto child = make_child_graph();

  wh::compose::graph parent{};
  REQUIRE(parent.add_subgraph(wh::compose::make_subgraph_node("child", std::move(child)))
              .has_value());
  REQUIRE(parent.add_entry_edge("child").has_value());
  REQUIRE(parent.add_exit_edge("child").has_value());
  REQUIRE(parent.compile().has_value());

  auto compiled = parent.compiled_node_by_key("child");
  REQUIRE(compiled.has_value());
  REQUIRE(compiled.value().get().meta.subgraph_snapshot.has_value());
  REQUIRE(compiled.value().get().meta.subgraph_restore_shape.has_value());
}
