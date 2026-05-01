#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph.hpp"
#include "wh/compose/graph/mode.hpp"
#include "wh/compose/node.hpp"

TEST_CASE("mode graph fixes runtime mode and graph name defaults",
          "[UT][wh/compose/graph/mode.hpp][mode_graph][condition][branch][boundary]") {
  wh::compose::mode_graph<wh::compose::graph_runtime_mode::dag> dag{};
  REQUIRE(dag.options().mode == wh::compose::graph_runtime_mode::dag);
  REQUIRE(dag.options().name == "dag");

  wh::compose::graph_compile_options options{};
  options.name = "x";
  wh::compose::mode_graph<wh::compose::graph_runtime_mode::pregel> pregel{std::move(options)};
  REQUIRE(pregel.options().mode == wh::compose::graph_runtime_mode::pregel);
  REQUIRE(pregel.options().name == "x");
}

TEST_CASE(
    "mode graph keeps fixed runtime mode through compile snapshot and release_graph",
    "[UT][wh/compose/graph/mode.hpp][mode_graph::release_graph][condition][branch][boundary]") {
  wh::compose::mode_graph<wh::compose::graph_runtime_mode::dag> dag{};
  REQUIRE(dag.add_passthrough(wh::compose::make_passthrough_node("worker")).has_value());
  REQUIRE(dag.add_entry_edge("worker").has_value());
  REQUIRE(dag.add_exit_edge("worker").has_value());
  REQUIRE(dag.compile().has_value());
  REQUIRE(dag.compiled());
  REQUIRE(dag.compile_options_snapshot().mode == wh::compose::graph_runtime_mode::dag);

  auto released = std::move(dag).release_graph();
  REQUIRE(released.options().mode == wh::compose::graph_runtime_mode::dag);
  REQUIRE(released.compiled());
}
