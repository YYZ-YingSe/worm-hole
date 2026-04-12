#include <catch2/catch_test_macros.hpp>

#include "helper/compose_graph_runtime_support.hpp"
#include "wh/compose/graph/detail/dag_entry.hpp"

namespace {

class run_state_probe final : public wh::compose::detail::invoke_runtime::run_state {
public:
  using wh::compose::detail::invoke_runtime::run_state::run_state;
  using wh::compose::detail::invoke_runtime::run_state::invoke_state;
};

} // namespace

TEST_CASE("dag entry initialization commits start routing and exposes ready worker frontier",
          "[UT][wh/compose/graph/detail/dag_entry.hpp][dag_run_state::initialize_dag_entry][condition][branch][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::dag, "dag_entry");
  REQUIRE(graph.has_value());

  wh::core::run_context context{};
  auto base = wh::testing::helper::make_base_run_state(
      graph.value(), wh::compose::graph_value{5}, context);
  wh::compose::detail::invoke_runtime::dag_run_state dag_state{std::move(base)};
  dag_state.initialize_dag_entry();

  auto worker_id = graph->node_id("worker");
  REQUIRE(worker_id.has_value());
  REQUIRE(dag_state.frontier().dequeue() ==
          std::optional<std::uint32_t>{worker_id.value()});
}

TEST_CASE("dag entry initialization drains to a single ready worker on the identity runtime graph",
          "[UT][wh/compose/graph/detail/dag_entry.hpp][dag_run_state::frontier][condition][branch][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::dag, "dag_entry_reset");
  REQUIRE(graph.has_value());

  auto worker_id = graph->node_id("worker");
  REQUIRE(worker_id.has_value());

  wh::core::run_context context{};
  run_state_probe base{
      &graph.value(),
      wh::compose::graph_value{7},
      context,
      wh::compose::graph_call_options{},
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}),
  };
  base.invoke_state().start_entry_selection =
      std::vector<std::uint32_t>{worker_id.value()};
  wh::compose::detail::invoke_runtime::dag_run_state dag_state{std::move(base)};

  dag_state.initialize_dag_entry();

  REQUIRE(dag_state.frontier().dequeue() ==
          std::optional<std::uint32_t>{worker_id.value()});
  REQUIRE_FALSE(dag_state.frontier().dequeue().has_value());
}
