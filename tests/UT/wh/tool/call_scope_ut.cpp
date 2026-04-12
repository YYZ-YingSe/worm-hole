#include <catch2/catch_test_macros.hpp>

#include "wh/tool/call_scope.hpp"

TEST_CASE("tool call scope materializes stable execution location",
          "[UT][wh/tool/call_scope.hpp][call_scope::location][boundary]") {
  wh::core::run_context context{};
  wh::tool::call_scope scope{
      .run = context,
      .component = "router",
      .implementation = "impl",
      .tool_name = "search",
      .call_id = "call-1",
  };

  const auto location = scope.location();
  REQUIRE(location.to_string() == "tool/search/call-1");
}

TEST_CASE("tool call scope preserves borrowed metadata and isolates call ids",
          "[UT][wh/tool/call_scope.hpp][call_scope][condition][branch]") {
  wh::core::run_context left{};
  wh::core::run_context right{};

  wh::tool::call_scope first{
      .run = left,
      .component = "router",
      .implementation = "sync",
      .tool_name = "search",
      .call_id = "call-a",
  };
  wh::tool::call_scope second{
      .run = right,
      .component = "router",
      .implementation = "sync",
      .tool_name = "search",
      .call_id = "call-b",
  };

  REQUIRE(&first.run == &left);
  REQUIRE(&second.run == &right);
  REQUIRE(first.component == "router");
  REQUIRE(first.implementation == "sync");
  REQUIRE(first.location().to_string() == "tool/search/call-a");
  REQUIRE(second.location().to_string() == "tool/search/call-b");
}
