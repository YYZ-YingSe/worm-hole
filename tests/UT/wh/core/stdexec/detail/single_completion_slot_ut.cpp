#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "wh/core/stdexec/detail/single_completion_slot.hpp"

TEST_CASE("single completion slot publishes takes resets and rejects duplicate ready state",
          "[UT][wh/core/stdexec/detail/single_completion_slot.hpp][single_completion_slot::publish][condition][branch][boundary]") {
  wh::core::detail::single_completion_slot<std::unique_ptr<int>> slot{};

  REQUIRE_FALSE(slot.ready());
  REQUIRE_FALSE(slot.take().has_value());

  REQUIRE(slot.publish(std::make_unique<int>(7)));
  REQUIRE(slot.ready());
  REQUIRE_FALSE(slot.publish(std::make_unique<int>(9)));

  auto first = slot.take();
  REQUIRE(first.has_value());
  REQUIRE(**first == 7);
  REQUIRE_FALSE(slot.ready());
  REQUIRE_FALSE(slot.take().has_value());

  REQUIRE(slot.publish(std::make_unique<int>(11)));
  REQUIRE(slot.ready());
  slot.reset();
  REQUIRE_FALSE(slot.ready());
  REQUIRE_FALSE(slot.take().has_value());

  REQUIRE(slot.publish(std::make_unique<int>(13)));
  auto second = slot.take();
  REQUIRE(second.has_value());
  REQUIRE(**second == 13);
}

TEST_CASE("single completion slot reset is harmless when empty and take clears readiness",
          "[UT][wh/core/stdexec/detail/single_completion_slot.hpp][single_completion_slot::take][condition][branch][boundary]") {
  wh::core::detail::single_completion_slot<int> slot{};

  slot.reset();
  REQUIRE_FALSE(slot.ready());
  REQUIRE_FALSE(slot.take().has_value());

  REQUIRE(slot.publish(21));
  REQUIRE(slot.ready());
  auto taken = slot.take();
  REQUIRE(taken.has_value());
  REQUIRE(*taken == 21);
  REQUIRE_FALSE(slot.ready());
}
