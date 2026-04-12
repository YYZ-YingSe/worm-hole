#include <catch2/catch_test_macros.hpp>

#include <type_traits>

#include "wh/compose/graph.hpp"

static_assert(std::same_as<wh::compose::dag,
                           wh::compose::mode_graph<wh::compose::graph_runtime_mode::dag>>);
static_assert(std::same_as<
              wh::compose::pregel,
              wh::compose::mode_graph<wh::compose::graph_runtime_mode::pregel>>);

TEST_CASE("compose graph facade exports reserved graph keys through the public header",
          "[UT][wh/compose/graph.hpp][graph_start_node_key][condition][branch][boundary]") {
  REQUIRE(wh::compose::graph_start_node_key == "__start__");
  REQUIRE(wh::compose::graph_end_node_key == "__end__");
  REQUIRE(wh::compose::graph_start_node_key != wh::compose::graph_end_node_key);
}

TEST_CASE("compose graph facade exports dag and pregel mode-specialized graph aliases",
          "[UT][wh/compose/graph.hpp][dag][condition][branch][boundary]") {
  wh::compose::dag dag{};
  wh::compose::pregel pregel{};

  REQUIRE(dag.options().mode == wh::compose::graph_runtime_mode::dag);
  REQUIRE(pregel.options().mode == wh::compose::graph_runtime_mode::pregel);
}

TEST_CASE("compose graph facade exposes call and compile option types",
          "[UT][wh/compose/graph.hpp][graph_call_options][condition][branch][boundary]") {
  wh::compose::graph_call_options call_options{};
  wh::compose::graph_compile_options compile_options{};

  REQUIRE_FALSE(call_options.record_transition_log);
  REQUIRE(call_options.isolate_debug_stream);
  REQUIRE_FALSE(call_options.external_interrupt_policy.has_value());
  REQUIRE(compile_options.mode == wh::compose::graph_runtime_mode::dag);
}
