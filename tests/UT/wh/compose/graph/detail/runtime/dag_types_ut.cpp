#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph/detail/runtime/dag_types.hpp"

TEST_CASE("dag runtime types reset branch decisions and preserve decided frontier bookkeeping",
          "[UT][wh/compose/graph/detail/runtime/dag_types.hpp][dag_schedule_state::mark_branch_decided][condition][branch][boundary]") {
  using namespace wh::compose::detail::input_runtime;

  REQUIRE(dag_edge_status::waiting == input_edge_status::waiting);
  REQUIRE(dag_ready_state::waiting != dag_ready_state::ready);

  dag_schedule_state schedule{};
  schedule.reset(4U);
  REQUIRE(schedule.branch_states.size() >= 4U);
  REQUIRE(schedule.decided_branch_nodes.empty());

  schedule.mark_branch_decided(2U, {3U, 5U});
  REQUIRE(schedule.decided_branch_nodes == std::vector<std::uint32_t>{2U});
  REQUIRE(schedule.branch_states[2U].decided);
  REQUIRE(schedule.branch_states[2U].selected_end_nodes_sorted ==
          std::vector<std::uint32_t>{3U, 5U});

  schedule.mark_branch_decided(2U, {7U});
  REQUIRE(schedule.decided_branch_nodes == std::vector<std::uint32_t>{2U});
  REQUIRE(schedule.branch_states[2U].selected_end_nodes_sorted ==
          std::vector<std::uint32_t>{7U});

  schedule.reset(4U);
  REQUIRE(schedule.decided_branch_nodes.empty());
  REQUIRE_FALSE(schedule.branch_states[2U].decided);
  REQUIRE(schedule.branch_states[2U].selected_end_nodes_sorted.empty());
}

TEST_CASE("dag runtime schedule reuses branch bookkeeping across multiple resets and unique nodes",
          "[UT][wh/compose/graph/detail/runtime/dag_types.hpp][dag_schedule_state::reset][condition][branch][boundary]") {
  using namespace wh::compose::detail::input_runtime;

  dag_schedule_state schedule{};
  schedule.reset(2U);
  schedule.mark_branch_decided(0U, {2U});
  schedule.mark_branch_decided(1U, {3U, 4U});

  REQUIRE(schedule.decided_branch_nodes ==
          std::vector<std::uint32_t>{0U, 1U});

  schedule.reset(1U);
  REQUIRE(schedule.branch_states.size() >= 2U);
  REQUIRE(schedule.decided_branch_nodes.empty());
  REQUIRE_FALSE(schedule.branch_states[0U].decided);
  REQUIRE_FALSE(schedule.branch_states[1U].decided);
  REQUIRE(schedule.branch_states[0U].selected_end_nodes_sorted.empty());
  REQUIRE(schedule.branch_states[1U].selected_end_nodes_sorted.empty());

  schedule.mark_branch_decided(1U, {9U});
  REQUIRE(schedule.decided_branch_nodes == std::vector<std::uint32_t>{1U});
  REQUIRE(schedule.branch_states[1U].selected_end_nodes_sorted ==
          std::vector<std::uint32_t>{9U});
}
