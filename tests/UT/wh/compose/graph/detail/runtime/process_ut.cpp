#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph/detail/runtime/process.hpp"

TEST_CASE("process runtime acquires binds and releases node local process state slots safely",
          "[UT][wh/compose/graph/detail/runtime/process.hpp][acquire_node_local_process_state][condition][branch][boundary]") {
  using namespace wh::compose::detail::process_runtime;

  node_local_process_state_slots slots(2U);
  wh::compose::graph_process_state parent{};

  auto invalid = acquire_node_local_process_state(slots, 9U, parent);
  REQUIRE(invalid.has_error());
  REQUIRE(invalid.error() == wh::core::errc::contract_violation);

  auto acquired = acquire_node_local_process_state(slots, 1U, parent);
  REQUIRE(acquired.has_value());
  REQUIRE(slots[1U].has_value());
  REQUIRE(slots[1U]->parent() == &parent);

  auto resolved = acquired->get(slots);
  REQUIRE(resolved.has_value());
  REQUIRE(&resolved->get() == &*slots[1U]);

  wh::compose::graph_process_state rebound{};
  bind_parent_process_state(rebound, &parent);
  REQUIRE(rebound.parent() == &parent);

  acquired->release(slots);
  REQUIRE_FALSE(slots[1U].has_value());
  auto released = acquired->get(slots);
  REQUIRE(released.has_error());
  REQUIRE(released.error() == wh::core::errc::contract_violation);

  scoped_node_local_process_state empty{};
  auto empty_get = empty.get(slots);
  REQUIRE(empty_get.has_error());
  REQUIRE(empty_get.error() == wh::core::errc::contract_violation);
}

TEST_CASE("process runtime distinguishes an empty slot from an invalid scoped token",
          "[UT][wh/compose/graph/detail/runtime/process.hpp][scoped_node_local_process_state::get][condition][branch][boundary]") {
  using namespace wh::compose::detail::process_runtime;

  node_local_process_state_slots slots(1U);
  scoped_node_local_process_state scoped{0U};

  auto missing = scoped.get(slots);
  REQUIRE(missing.has_error());
  REQUIRE(missing.error() == wh::core::errc::not_found);

  scoped.release(slots);
  REQUIRE_FALSE(slots[0U].has_value());

  scoped_node_local_process_state invalid{};
  auto invalid_result = invalid.get(slots);
  REQUIRE(invalid_result.has_error());
  REQUIRE(invalid_result.error() == wh::core::errc::contract_violation);
}
