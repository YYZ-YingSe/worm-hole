#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph/detail/runtime/pregel_types.hpp"

TEST_CASE("pregel runtime delivery store stages current and next superstep frontier safely",
          "[UT][wh/compose/graph/detail/runtime/pregel_types.hpp][pregel_delivery_store::advance_superstep][condition][branch][boundary]") {
  using namespace wh::compose::detail::input_runtime;

  REQUIRE(pregel_ready_state::ready != pregel_ready_state::skipped);

  pregel_node_inputs inputs{};
  inputs.control_edges = {1U};
  inputs.data_edges = {2U};
  inputs.reset();
  REQUIRE(inputs.control_edges.empty());
  REQUIRE(inputs.data_edges.empty());

  pregel_delivery_store store{};
  store.reset(4U);
  REQUIRE(store.current.size() >= 4U);
  REQUIRE(store.next.size() >= 4U);
  REQUIRE(store.current_frontier().empty());

  store.stage_current_node(1U);
  store.stage_current_control(1U, 10U);
  store.stage_current_data(1U, 20U);
  REQUIRE(store.current_frontier() == std::vector<std::uint32_t>{1U});
  REQUIRE(store.current[1U].control_edges == std::vector<std::uint32_t>{10U});
  REQUIRE(store.current[1U].data_edges == std::vector<std::uint32_t>{20U});

  store.stage_next_node(2U);
  store.stage_next_control(2U, 30U);
  store.stage_next_data(2U, 40U);
  REQUIRE(store.next_nodes == std::vector<std::uint32_t>{2U});

  store.clear_current_node(1U);
  REQUIRE(store.current[1U].control_edges.empty());
  REQUIRE(store.current[1U].data_edges.empty());
  REQUIRE_FALSE(store.current_enqueued.test(1U));

  store.stage_current_node(1U);
  auto advanced = store.advance_superstep();
  REQUIRE(advanced == std::vector<std::uint32_t>{2U});
  REQUIRE(store.current_frontier() == std::vector<std::uint32_t>{2U});
  REQUIRE(store.current[2U].control_edges == std::vector<std::uint32_t>{30U});
  REQUIRE(store.current[2U].data_edges == std::vector<std::uint32_t>{40U});
  REQUIRE(store.next_nodes.empty());
}

TEST_CASE("pregel runtime delivery store advances into an empty next frontier and can be reused",
          "[UT][wh/compose/graph/detail/runtime/pregel_types.hpp][pregel_delivery_store::reset][condition][branch][boundary]") {
  using namespace wh::compose::detail::input_runtime;

  pregel_delivery_store store{};
  store.reset(3U);
  store.stage_current_control(1U, 9U);
  store.stage_current_data(1U, 10U);

  const auto empty_advance = store.advance_superstep();
  REQUIRE(empty_advance.empty());
  REQUIRE(store.current_frontier().empty());
  REQUIRE(store.current[1U].control_edges.empty());
  REQUIRE(store.current[1U].data_edges.empty());

  store.stage_next_control(1U, 11U);
  auto next_wave = store.advance_superstep();
  REQUIRE(next_wave == std::vector<std::uint32_t>{1U});
  REQUIRE(store.current_frontier() == std::vector<std::uint32_t>{1U});
  REQUIRE(store.current[1U].control_edges == std::vector<std::uint32_t>{11U});

  store.reset(1U);
  REQUIRE(store.current_frontier().empty());
  REQUIRE(store.next_nodes.empty());
}
