#include <catch2/catch_test_macros.hpp>

#include "wh/compose/runtime.hpp"

TEST_CASE("compose runtime facade exposes graph process state storage helpers",
          "[UT][wh/compose/runtime.hpp][graph_process_state::emplace][condition][branch][boundary]") {
  wh::compose::graph_process_state state{};
  REQUIRE(state.emplace<int>(1).has_value());
  REQUIRE(state.get<int>().has_value());
  REQUIRE(state.get<int>().value().get() == 1);
}

TEST_CASE("compose runtime facade exposes interrupt helpers through the public header",
          "[UT][wh/compose/runtime.hpp][make_interrupt_id][condition][branch][boundary]") {
  const auto first = wh::compose::make_interrupt_id();
  const auto second = wh::compose::make_interrupt_id();
  const auto signal = wh::compose::make_interrupt_signal(
      wh::core::address{"graph", "worker"}, std::string{"pause"},
      wh::compose::graph_value{7});

  REQUIRE_FALSE(first.empty());
  REQUIRE_FALSE(second.empty());
  REQUIRE(first != second);
  REQUIRE(signal.location.to_string() == "graph/worker");
}
