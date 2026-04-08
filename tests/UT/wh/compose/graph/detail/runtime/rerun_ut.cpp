#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph/detail/runtime/rerun.hpp"

TEST_CASE("rerun state tracks presence lookup restore flags and active counts across resets",
          "[UT][wh/compose/graph/detail/runtime/rerun.hpp][rerun_state::store][condition][branch][boundary]") {
  wh::compose::detail::runtime_state::rerun_state rerun{};
  rerun.reset(4U);
  REQUIRE(rerun.active_count() == 0U);
  REQUIRE_FALSE(rerun.contains(1U));
  REQUIRE(rerun.find(1U) == nullptr);
  REQUIRE_FALSE(rerun.restored(1U));

  rerun.store(1U, wh::compose::graph_value{9});
  REQUIRE(rerun.contains(1U));
  REQUIRE(rerun.active_count() == 1U);
  auto *stored = rerun.find(1U);
  REQUIRE(stored != nullptr);
  REQUIRE(*wh::core::any_cast<int>(stored) == 9);

  rerun.store(1U, wh::compose::graph_value{10});
  REQUIRE(rerun.active_count() == 1U);
  REQUIRE(*wh::core::any_cast<int>(rerun.find(1U)) == 10);

  rerun.mark_restored(1U);
  REQUIRE(rerun.restored(1U));
  REQUIRE_FALSE(rerun.restored(9U));

  rerun.store(3U, wh::compose::graph_value{12});
  REQUIRE(rerun.active_count() == 2U);
  REQUIRE(rerun.contains(3U));

  rerun.reset(2U);
  REQUIRE(rerun.active_count() == 0U);
  REQUIRE_FALSE(rerun.contains(1U));
  REQUIRE(rerun.find(1U) == nullptr);
  REQUIRE_FALSE(rerun.restored(1U));
}

TEST_CASE("rerun state exposes const lookup and rejects out-of-range access after shrink resets",
          "[UT][wh/compose/graph/detail/runtime/rerun.hpp][rerun_state::find][condition][branch][boundary]") {
  wh::compose::detail::runtime_state::rerun_state rerun{};
  rerun.reset(1U);
  rerun.store(0U, wh::compose::graph_value{4});

  const auto &const_rerun = rerun;
  const auto *stored = const_rerun.find(0U);
  REQUIRE(stored != nullptr);
  REQUIRE(*wh::core::any_cast<int>(stored) == 4);
  REQUIRE(const_rerun.find(5U) == nullptr);
  REQUIRE_FALSE(const_rerun.restored(5U));

  rerun.reset(0U);
  REQUIRE(rerun.active_count() == 0U);
  REQUIRE(rerun.find(0U) == nullptr);
  REQUIRE_FALSE(rerun.contains(0U));
}
