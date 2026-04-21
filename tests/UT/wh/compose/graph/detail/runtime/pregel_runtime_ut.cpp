#include <catch2/catch_test_macros.hpp>

#include "helper/compose_graph_runtime_support.hpp"
#include "wh/compose/graph/detail/runtime/pregel_runtime.hpp"

TEST_CASE("pregel runtime sizes delivery storage and seeds current frontier from start entry",
          "[UT][wh/compose/graph/detail/runtime/"
          "pregel_runtime.hpp][pregel_runtime::initialize_entry][condition][branch][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::pregel, "pregel_runtime");
  REQUIRE(graph.has_value());

  wh::core::run_context context{};
  auto base = wh::testing::helper::make_invoke_session(graph.value(), wh::compose::graph_value{21},
                                                       context);
  wh::compose::detail::invoke_runtime::pregel_runtime pregel_state{std::move(base)};

  pregel_state.initialize_entry();
  auto worker_id = graph->node_id("worker");
  REQUIRE(worker_id.has_value());
  REQUIRE(pregel_state.pregel_delivery().current_frontier() ==
          std::vector<std::uint32_t>{worker_id.value()});
}

TEST_CASE("pregel runtime starts empty and clears injected start selections during entry init",
          "[UT][wh/compose/graph/detail/runtime/"
          "pregel_runtime.hpp][pregel_runtime::pregel_delivery][condition][branch][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::pregel, "pregel_runtime_probe");
  REQUIRE(graph.has_value());

  auto worker_id = graph->node_id("worker");
  REQUIRE(worker_id.has_value());

  wh::core::run_context context{};
  auto base = wh::testing::helper::make_invoke_session(graph.value(), wh::compose::graph_value{22},
                                                       context);
  wh::compose::detail::invoke_runtime::pregel_runtime pregel_state{std::move(base)};

  REQUIRE(pregel_state.pregel_delivery().current_frontier().empty());
  pregel_state.initialize_entry();

  REQUIRE(pregel_state.pregel_delivery().current_frontier() ==
          std::vector<std::uint32_t>{worker_id.value()});
}
