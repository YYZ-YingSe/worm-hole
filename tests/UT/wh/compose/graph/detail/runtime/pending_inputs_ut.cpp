#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph/detail/runtime/pending_inputs.hpp"

TEST_CASE("pending input store tracks presence lookup restore flags and active counts across resets",
          "[UT][wh/compose/graph/detail/runtime/pending_inputs.hpp][pending_inputs::store_input][condition][branch][boundary]") {
  wh::compose::detail::runtime_state::pending_inputs inputs{};
  inputs.reset(4U);
  REQUIRE(inputs.active_input_count() == 0U);
  REQUIRE_FALSE(inputs.contains_input(1U));
  REQUIRE(inputs.find_input(1U) == nullptr);
  REQUIRE_FALSE(inputs.restored_node(1U));

  inputs.store_input(1U, wh::compose::graph_value{9});
  REQUIRE(inputs.contains_input(1U));
  REQUIRE(inputs.active_input_count() == 1U);
  auto *stored = inputs.find_input(1U);
  REQUIRE(stored != nullptr);
  REQUIRE(*wh::core::any_cast<int>(stored) == 9);

  inputs.store_input(1U, wh::compose::graph_value{10});
  REQUIRE(inputs.active_input_count() == 1U);
  REQUIRE(*wh::core::any_cast<int>(inputs.find_input(1U)) == 10);

  inputs.mark_restored_node(1U);
  REQUIRE(inputs.restored_node(1U));
  REQUIRE_FALSE(inputs.restored_node(9U));

  inputs.store_input(3U, wh::compose::graph_value{12});
  REQUIRE(inputs.active_input_count() == 2U);
  REQUIRE(inputs.contains_input(3U));

  inputs.reset(2U);
  REQUIRE(inputs.active_input_count() == 0U);
  REQUIRE_FALSE(inputs.contains_input(1U));
  REQUIRE(inputs.find_input(1U) == nullptr);
  REQUIRE_FALSE(inputs.restored_node(1U));
}

TEST_CASE("pending input store exposes const lookup and rejects out-of-range access after shrink resets",
          "[UT][wh/compose/graph/detail/runtime/pending_inputs.hpp][pending_inputs::find_input][condition][branch][boundary]") {
  wh::compose::detail::runtime_state::pending_inputs inputs{};
  inputs.reset(1U);
  inputs.store_input(0U, wh::compose::graph_value{4});

  const auto &const_inputs = inputs;
  const auto *stored = const_inputs.find_input(0U);
  REQUIRE(stored != nullptr);
  REQUIRE(*wh::core::any_cast<int>(stored) == 4);
  REQUIRE(const_inputs.find_input(5U) == nullptr);
  REQUIRE_FALSE(const_inputs.restored_node(5U));

  inputs.reset(0U);
  REQUIRE(inputs.active_input_count() == 0U);
  REQUIRE(inputs.find_input(0U) == nullptr);
  REQUIRE_FALSE(inputs.contains_input(0U));
}
