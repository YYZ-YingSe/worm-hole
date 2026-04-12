#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph/policy.hpp"

TEST_CASE("compose graph policy enums expose expected stable ordinals",
          "[UT][wh/compose/graph/policy.hpp][graph_runtime_mode][boundary]") {
  REQUIRE(static_cast<int>(wh::compose::graph_runtime_mode::dag) == 0);
  REQUIRE(static_cast<int>(wh::compose::graph_runtime_mode::pregel) == 1);
  REQUIRE(static_cast<int>(wh::compose::graph_dispatch_policy::same_wave) == 0);
  REQUIRE(static_cast<int>(wh::compose::graph_trigger_mode::all_predecessors) ==
          1);
  REQUIRE(static_cast<int>(wh::compose::graph_fan_in_policy::require_all_sources) ==
          1);
}

TEST_CASE("compose graph policy enums keep uint8 underlying representation and full enum ordering",
          "[UT][wh/compose/graph/policy.hpp][graph_dispatch_policy][condition][branch]") {
  STATIC_REQUIRE(std::same_as<std::underlying_type_t<wh::compose::graph_runtime_mode>,
                              std::uint8_t>);
  STATIC_REQUIRE(std::same_as<std::underlying_type_t<wh::compose::graph_dispatch_policy>,
                              std::uint8_t>);
  STATIC_REQUIRE(std::same_as<std::underlying_type_t<wh::compose::graph_trigger_mode>,
                              std::uint8_t>);
  STATIC_REQUIRE(std::same_as<std::underlying_type_t<wh::compose::graph_fan_in_policy>,
                              std::uint8_t>);

  REQUIRE(static_cast<int>(wh::compose::graph_dispatch_policy::next_wave) == 1);
  REQUIRE(static_cast<int>(wh::compose::graph_trigger_mode::any_predecessor) == 0);
  REQUIRE(static_cast<int>(wh::compose::graph_fan_in_policy::allow_partial) == 0);
}
