#include <catch2/catch_test_macros.hpp>

#include "wh/internal/stacktrace.hpp"

TEST_CASE("stacktrace capture returns a best effort diagnostic string",
          "[UT][wh/internal/stacktrace.hpp][capture_call_stack][condition][branch][boundary]") {
  const auto stack = wh::internal::capture_call_stack();
  REQUIRE_FALSE(stack.empty());
}

TEST_CASE("stacktrace capture remains stable across repeated calls",
          "[UT][wh/internal/stacktrace.hpp][capture_call_stack][condition][branch][boundary][repeat]") {
  const auto first = wh::internal::capture_call_stack();
  const auto second = wh::internal::capture_call_stack();
  REQUIRE_FALSE(first.empty());
  REQUIRE_FALSE(second.empty());
}
