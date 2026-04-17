#include <catch2/catch_test_macros.hpp>

#include "helper/compose_graph_runtime_support.hpp"
#include "wh/compose/graph/detail/pregel_entry.hpp"

namespace {

class invoke_session_probe final : public wh::compose::detail::invoke_runtime::invoke_session {
public:
  using wh::compose::detail::invoke_runtime::invoke_session::invoke_session;
  using wh::compose::detail::invoke_runtime::invoke_session::invoke_state;
};

} // namespace

TEST_CASE("pregel entry initialization seeds current delivery frontier for the first worker wave",
          "[UT][wh/compose/graph/detail/pregel_entry.hpp][pregel_runtime::initialize_entry][condition][branch][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::pregel, "pregel_entry");
  REQUIRE(graph.has_value());

  wh::core::run_context context{};
  auto base = wh::testing::helper::make_invoke_session(
      graph.value(), wh::compose::graph_value{6}, context);
  wh::compose::detail::invoke_runtime::pregel_runtime pregel_state{
      std::move(base)};
  pregel_state.initialize_entry();

  auto worker_id = graph->node_id("worker");
  REQUIRE(worker_id.has_value());
  REQUIRE(pregel_state.pregel_delivery().current_frontier() ==
          std::vector<std::uint32_t>{worker_id.value()});
}

TEST_CASE("pregel entry initialization leaves a single seeded worker frontier on the identity graph",
          "[UT][wh/compose/graph/detail/pregel_entry.hpp][pregel_runtime::pregel_delivery][condition][branch][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::pregel, "pregel_entry_reset");
  REQUIRE(graph.has_value());

  auto worker_id = graph->node_id("worker");
  REQUIRE(worker_id.has_value());

  wh::core::run_context context{};
  invoke_session_probe base{
      &graph.value(),
      wh::compose::graph_value{8},
      context,
      wh::compose::graph_call_options{},
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}),
  };
  base.invoke_state().start_entry_selection =
      std::vector<std::uint32_t>{worker_id.value()};
  wh::compose::detail::invoke_runtime::pregel_runtime pregel_state{
      std::move(base)};

  pregel_state.initialize_entry();

  REQUIRE(pregel_state.pregel_delivery().current_frontier() ==
          std::vector<std::uint32_t>{worker_id.value()});
  REQUIRE(pregel_state.pregel_delivery().next_nodes.empty());
}
