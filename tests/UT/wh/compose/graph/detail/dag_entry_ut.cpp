#include <catch2/catch_test_macros.hpp>

#include "helper/compose_graph_runtime_support.hpp"
#include "wh/compose/graph/detail/dag_entry.hpp"

namespace {

class invoke_session_probe final : public wh::compose::detail::invoke_runtime::invoke_session {
public:
  using wh::compose::detail::invoke_runtime::invoke_session::invoke_session;
  using wh::compose::detail::invoke_runtime::invoke_session::invoke_state;
};

} // namespace

TEST_CASE("dag entry initialization commits start routing and exposes ready worker frontier",
          "[UT][wh/compose/graph/detail/dag_entry.hpp][dag_runtime::initialize_entry][condition][branch][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::dag, "dag_entry");
  REQUIRE(graph.has_value());

  wh::core::run_context context{};
  auto base = wh::testing::helper::make_invoke_session(
      graph.value(), wh::compose::graph_value{5}, context);
  wh::compose::detail::invoke_runtime::dag_runtime dag_state{std::move(base)};
  dag_state.initialize_entry();

  auto worker_id = graph->node_id("worker");
  REQUIRE(worker_id.has_value());
  REQUIRE(dag_state.frontier().dequeue() ==
          std::optional<std::uint32_t>{worker_id.value()});
}

TEST_CASE("dag entry initialization drains to a single ready worker on the identity runtime graph",
          "[UT][wh/compose/graph/detail/dag_entry.hpp][dag_runtime::frontier][condition][branch][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::dag, "dag_entry_reset");
  REQUIRE(graph.has_value());

  auto worker_id = graph->node_id("worker");
  REQUIRE(worker_id.has_value());

  wh::core::run_context context{};
  invoke_session_probe base{
      &graph.value(),
      wh::compose::graph_value{7},
      context,
      wh::compose::graph_call_options{},
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}),
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}),
  };
  base.invoke_state().start_entry_selection =
      std::vector<std::uint32_t>{worker_id.value()};
  wh::compose::detail::invoke_runtime::dag_runtime dag_state{std::move(base)};

  dag_state.initialize_entry();

  REQUIRE(dag_state.frontier().dequeue() ==
          std::optional<std::uint32_t>{worker_id.value()});
  REQUIRE_FALSE(dag_state.frontier().dequeue().has_value());
}
