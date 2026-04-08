#include <catch2/catch_test_macros.hpp>

#include <type_traits>

#include "wh/compose/graph/dag.hpp"

TEST_CASE("dag facade aliases dag-specialized mode graph",
          "[UT][wh/compose/graph/dag.hpp][dag][boundary]") {
  STATIC_REQUIRE(std::same_as<wh::compose::dag,
                              wh::compose::mode_graph<wh::compose::graph_runtime_mode::dag>>);
}

TEST_CASE("dag facade fixes graph runtime mode for default and custom options",
          "[UT][wh/compose/graph/dag.hpp][dag][condition][branch]") {
  wh::compose::dag default_dag{};
  REQUIRE(default_dag.options().mode == wh::compose::graph_runtime_mode::dag);
  REQUIRE(default_dag.options().name == "dag");

  wh::compose::graph_compile_options options{};
  options.name = "custom";
  options.mode = wh::compose::graph_runtime_mode::pregel;

  wh::compose::dag customized{options};
  REQUIRE(customized.options().name == "custom");
  REQUIRE(customized.options().mode == wh::compose::graph_runtime_mode::dag);
}
