#include <catch2/catch_test_macros.hpp>

#include "helper/compose_graph_runtime_support.hpp"
#include "wh/compose/graph/detail/pregel_loop.hpp"
#include "wh/compose/graph/detail/start.hpp"

namespace {

class invoke_session_probe final
    : public wh::compose::detail::invoke_runtime::invoke_session {
public:
  using wh::compose::detail::invoke_runtime::invoke_session::invoke_session;

  auto mark_completed(const std::uint32_t node_id) -> void {
    auto updated = state_table_.update(
        node_id, wh::compose::graph_node_lifecycle_state::completed, 0U,
        std::nullopt);
    REQUIRE(updated.has_value());
  }
};

} // namespace

TEST_CASE("pregel loop prepares launch actions for current frontier nodes",
          "[UT][wh/compose/graph/detail/pregel_loop.hpp][pregel_runtime::take_ready_action][condition][branch][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::pregel, "pregel_loop");
  REQUIRE(graph.has_value());

  wh::core::run_context context{};
  auto base = wh::testing::helper::make_invoke_session(
      graph.value(), wh::compose::graph_value{12}, context);
  wh::compose::detail::invoke_runtime::pregel_runtime pregel_state{
      std::move(base)};
  pregel_state.initialize_entry();

  auto worker_id = graph->node_id("worker");
  REQUIRE(worker_id.has_value());

  auto action = pregel_state.take_ready_action(worker_id.value(), 1U);
  REQUIRE(action.action ==
          wh::compose::detail::invoke_runtime::pregel_action::kind::launch);
  REQUIRE(action.node_id == worker_id.value());
  REQUIRE(action.frame.has_value());
  REQUIRE(action.cause.step == 1U);
}

TEST_CASE("pregel loop still launches re-enqueued nodes after a prior lifecycle completion",
          "[UT][wh/compose/graph/detail/pregel_loop.hpp][pregel_runtime::take_ready_action][condition][loop][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::pregel, "pregel_loop_reenqueue");
  REQUIRE(graph.has_value());

  auto worker_id = graph->node_id("worker");
  REQUIRE(worker_id.has_value());

  wh::core::run_context context{};
  invoke_session_probe base{
      &graph.value(),
      wh::compose::graph_value{21},
      context,
      wh::compose::graph_call_options{},
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}),
  };
  base.mark_completed(worker_id.value());
  wh::compose::detail::invoke_runtime::pregel_runtime pregel_state{
      std::move(base)};
  pregel_state.initialize_entry();

  auto action = pregel_state.take_ready_action(worker_id.value(), 2U);
  REQUIRE(action.action ==
          wh::compose::detail::invoke_runtime::pregel_action::kind::launch);
  REQUIRE(action.node_id == worker_id.value());
  REQUIRE(action.frame.has_value());
  REQUIRE(action.cause.step == 2U);
}

TEST_CASE("pregel loop skips nodes that no longer satisfy current-step readiness",
          "[UT][wh/compose/graph/detail/pregel_loop.hpp][pregel_runtime::take_ready_action][condition][branch][boundary]") {
  auto graph = wh::testing::helper::make_runtime_identity_graph(
      wh::compose::graph_runtime_mode::pregel, "pregel_loop_skip");
  REQUIRE(graph.has_value());

  wh::core::run_context context{};
  auto base = wh::testing::helper::make_invoke_session(
      graph.value(), wh::compose::graph_value{13}, context);
  wh::compose::detail::invoke_runtime::pregel_runtime pregel_state{
      std::move(base)};
  pregel_state.initialize_entry();

  auto worker_id = graph->node_id("worker");
  REQUIRE(worker_id.has_value());

  pregel_state.pregel_delivery().clear_current_node(worker_id.value());

  const auto action = pregel_state.take_ready_action(worker_id.value(), 2U);
  REQUIRE(action.action ==
          wh::compose::detail::invoke_runtime::pregel_action::kind::skip);
  REQUIRE(action.node_id == worker_id.value());
  REQUIRE(action.cause.step == 2U);
  REQUIRE(action.cause.node_key == "worker");
}
