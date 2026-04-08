#include <catch2/catch_test_macros.hpp>

#include <array>

#include "wh/tool/interrupt.hpp"

TEST_CASE("tool interrupt converts to core signals and injects resume payloads",
          "[UT][wh/tool/interrupt.hpp][to_interrupt_signal][branch][boundary]") {
  wh::tool::tool_interrupt interrupt{
      .interrupt_id = "int-1",
      .location = wh::core::make_address({"tool", "search"}),
      .payload = wh::core::any{7},
  };

  const auto signal = wh::tool::to_interrupt_signal(interrupt);
  REQUIRE(signal.interrupt_id == "int-1");
  REQUIRE(signal.location == interrupt.location);
  REQUIRE(wh::core::any_cast<int>(&signal.state) != nullptr);
  REQUIRE(*wh::core::any_cast<int>(&signal.state) == 7);

  const auto roundtrip = wh::tool::from_interrupt_signal(signal);
  REQUIRE(roundtrip.interrupt_id == "int-1");
  REQUIRE(roundtrip.location == interrupt.location);
  REQUIRE(*wh::core::any_cast<int>(&roundtrip.payload) == 7);

  wh::core::resume_state state{};
  auto injected = wh::tool::inject_resume_data(interrupt, state);
  REQUIRE(injected.has_value());
  REQUIRE(state.contains_interrupt_id("int-1"));
  REQUIRE(wh::tool::is_resume_target(interrupt, state));
  REQUIRE_FALSE(
      wh::tool::is_resume_target(
          wh::tool::tool_interrupt{
              .interrupt_id = "int-2",
              .location = wh::core::make_address({"tool"}),
          },
          state, true));
}

TEST_CASE("tool interrupt aggregation reports root causes and rejects empty sets",
          "[UT][wh/tool/interrupt.hpp][aggregate_interrupts][branch]") {
  const std::array causes{
      wh::core::error_code{},
      wh::core::make_error(wh::core::errc::contract_violation),
      wh::core::make_error(wh::core::errc::canceled),
  };
  const auto root = wh::tool::infer_root_cause(causes);
  REQUIRE(root.has_value());
  REQUIRE(*root == wh::core::errc::contract_violation);

  std::array<wh::tool::tool_interrupt, 2> interrupts{
      wh::tool::tool_interrupt{
          .interrupt_id = "a",
          .location = wh::core::make_address({"tool", "a"}),
      },
      wh::tool::tool_interrupt{
          .interrupt_id = "b",
          .location = wh::core::make_address({"tool", "b"}),
      },
  };
  auto aggregated = wh::tool::aggregate_interrupts(interrupts, root);
  REQUIRE(aggregated.has_value());
  REQUIRE(aggregated.value().interrupts.size() == 2U);
  REQUIRE(aggregated.value().root_cause == root);

  auto empty = wh::tool::aggregate_interrupts(
      std::span<const wh::tool::tool_interrupt>{});
  REQUIRE(empty.has_error());
  REQUIRE(empty.error() == wh::core::errc::not_found);
}

TEST_CASE("tool interrupt helpers return no root cause for empty or all-ok sets",
          "[UT][wh/tool/interrupt.hpp][infer_root_cause][condition][boundary]") {
  const std::array<wh::core::error_code, 0> no_causes{};
  REQUIRE_FALSE(wh::tool::infer_root_cause(no_causes).has_value());

  const std::array ok_causes{wh::core::error_code{}, wh::core::error_code{}};
  REQUIRE_FALSE(wh::tool::infer_root_cause(ok_causes).has_value());

  const std::array interrupts{
      wh::tool::tool_interrupt{
          .interrupt_id = "tool-a",
          .location = wh::core::make_address({"tool", "a"}),
      },
  };
  auto aggregated = wh::tool::aggregate_interrupts(interrupts);
  REQUIRE(aggregated.has_value());
  REQUIRE_FALSE(aggregated.value().root_cause.has_value());
}
