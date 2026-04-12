#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "wh/compose/node/tools.hpp"

TEST_CASE("tools facade reexports tools builder surface with default value contracts",
          "[UT][wh/compose/node/tools.hpp][make_tools_node][condition][branch][boundary]") {
  auto node = wh::compose::make_tools_node("tools", wh::compose::tool_registry{});
  REQUIRE(node.key() == "tools");
  REQUIRE(node.input_contract() == wh::compose::node_contract::value);
  REQUIRE(node.output_contract() == wh::compose::node_contract::value);
}

TEST_CASE("tools facade also reexports tools contract surface types",
          "[UT][wh/compose/node/tools.hpp][tools_options][condition][boundary]") {
  STATIC_REQUIRE(std::is_same_v<decltype(wh::compose::tool_batch{}.calls),
                                std::vector<wh::compose::tool_call>>);

  wh::compose::tools_options options{};
  REQUIRE(options.sequential);
  REQUIRE_FALSE(options.missing.has_value());
  REQUIRE(options.middleware.empty());
}
