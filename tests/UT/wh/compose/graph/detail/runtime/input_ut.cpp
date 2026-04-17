#include <catch2/catch_test_macros.hpp>

#include <type_traits>
#include <vector>

#include "wh/compose/graph/detail/runtime/input.hpp"

TEST_CASE("runtime input umbrella re-exports the shared dag and runtime aliases",
          "[UT][wh/compose/graph/detail/runtime/input.hpp][input_runtime::dag_node_phase][condition][branch][boundary]") {
  using namespace wh::compose::detail::input_runtime;

  static_assert(std::same_as<dag_node_phase, dag_node_phase>);
  static_assert(std::same_as<edge_status, dag_edge_status>);
  static_assert(std::same_as<ready_state, dag_ready_state>);
  static_assert(std::same_as<branch_state, dag_branch_state>);
  static_assert(std::same_as<io_storage, runtime_io_storage>);
  static_assert(std::same_as<dag_schedule, dag_schedule_state>);

  std::vector<dag_node_phase> progress(2U, dag_node_phase::pending);
  REQUIRE(progress.size() >= 2U);
  REQUIRE(progress[0U] == dag_node_phase::pending);

  dag_schedule schedule{};
  schedule.reset(2U);
  schedule.mark_branch_decided(1U, {4U});
  REQUIRE(schedule.branch_states[1U].decided);
}

TEST_CASE("runtime input umbrella also exposes pregel delivery and value input helpers through one include",
          "[UT][wh/compose/graph/detail/runtime/input.hpp][input_runtime::pregel_delivery_store][condition][branch][boundary]") {
  using namespace wh::compose::detail::input_runtime;

  pregel_delivery_store delivery{};
  delivery.reset(2U);
  delivery.stage_current_node(1U);
  REQUIRE(delivery.current_frontier() == std::vector<std::uint32_t>{1U});

  value_batch batch{};
  batch.form = value_input_form::direct;
  value_input entry{};
  entry.source_id = 1U;
  entry.edge_id = 9U;
  entry.owned = wh::compose::graph_value{6};
  REQUIRE(append_value_input(batch, std::move(entry)).has_value());
  REQUIRE(batch.single.has_value());

  auto materialized = materialize_value_input(*batch.single);
  REQUIRE(materialized.has_value());
  REQUIRE(*wh::core::any_cast<int>(&materialized.value()) == 6);
}
