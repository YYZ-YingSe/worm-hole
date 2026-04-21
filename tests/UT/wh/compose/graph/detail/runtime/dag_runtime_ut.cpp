#include <catch2/catch_test_macros.hpp>

#include "helper/compose_graph_runtime_support.hpp"
#include "wh/compose/graph/detail/runtime/dag_runtime.hpp"

TEST_CASE("dag runtime sizes branch storage and seeds frontier from start entry",
          "[UT][wh/compose/graph/detail/runtime/"
          "dag_runtime.hpp][dag_runtime::initialize_entry][condition][branch][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::dag, "dag_runtime");
  REQUIRE(graph.has_value());

  wh::core::run_context context{};
  auto base = wh::testing::helper::make_invoke_session(graph.value(), wh::compose::graph_value{13},
                                                       context);
  wh::compose::detail::invoke_runtime::dag_runtime dag_state{std::move(base)};

  REQUIRE(dag_state.branch_states().size() >= graph->snapshot_view().nodes.size());
  dag_state.initialize_entry();

  auto worker_id = graph->node_id("worker");
  REQUIRE(worker_id.has_value());
  REQUIRE(dag_state.frontier().dequeue() == std::optional<std::uint32_t>{worker_id.value()});
  REQUIRE_FALSE(dag_state.promote_next_wave());
}

TEST_CASE(
    "dag runtime starts with an empty frontier and clears injected start selections after init",
    "[UT][wh/compose/graph/detail/runtime/"
    "dag_runtime.hpp][dag_runtime::frontier][condition][branch][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::dag, "dag_runtime_probe");
  REQUIRE(graph.has_value());

  auto worker_id = graph->node_id("worker");
  REQUIRE(worker_id.has_value());

  wh::core::run_context context{};
  auto base = wh::testing::helper::make_invoke_session(graph.value(), wh::compose::graph_value{17},
                                                       context);
  wh::compose::detail::invoke_runtime::dag_runtime dag_state{std::move(base)};

  REQUIRE_FALSE(dag_state.frontier().dequeue().has_value());
  dag_state.initialize_entry();

  REQUIRE(dag_state.frontier().dequeue() == std::optional<std::uint32_t>{worker_id.value()});
}
