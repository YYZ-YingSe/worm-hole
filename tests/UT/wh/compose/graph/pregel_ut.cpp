#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph/pregel.hpp"

TEST_CASE("pregel aliases the fixed pregel mode facade",
          "[UT][wh/compose/graph/pregel.hpp][pregel][condition][boundary]") {
  STATIC_REQUIRE(std::is_default_constructible_v<wh::compose::pregel>);
  STATIC_REQUIRE(std::same_as<wh::compose::pregel,
                              wh::compose::mode_graph<wh::compose::graph_runtime_mode::pregel>>);

  wh::compose::pregel graph{};
  REQUIRE(graph.options().mode == wh::compose::graph_runtime_mode::pregel);
  REQUIRE(graph.options().name == "pregel");
  REQUIRE(graph.graph_view().boundary().input == wh::compose::node_contract::value);
}

TEST_CASE("pregel constructor normalizes runtime mode and preserves caller naming",
          "[UT][wh/compose/graph/pregel.hpp][pregel][branch][boundary]") {
  wh::compose::graph_compile_options options{};
  options.name = "custom-pregel";
  options.mode = wh::compose::graph_runtime_mode::dag;

  wh::compose::pregel graph{std::move(options)};
  REQUIRE(graph.options().mode == wh::compose::graph_runtime_mode::pregel);
  REQUIRE(graph.options().name == "custom-pregel");

  REQUIRE(graph.add_passthrough("worker").has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());
}
