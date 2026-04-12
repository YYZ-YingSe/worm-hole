#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <type_traits>

#include "wh/compose/graph/keys.hpp"

TEST_CASE("compose graph start key exposes a stable reserved name",
          "[UT][wh/compose/graph/keys.hpp][graph_start_node_key][condition][branch][boundary]") {
  REQUIRE(wh::compose::graph_start_node_key == "__start__");
}

TEST_CASE("compose graph end key exposes a stable reserved name distinct from start",
          "[UT][wh/compose/graph/keys.hpp][graph_end_node_key][condition][branch][boundary]") {
  REQUIRE(wh::compose::graph_end_node_key == "__end__");
  REQUIRE(wh::compose::graph_start_node_key != wh::compose::graph_end_node_key);
  STATIC_REQUIRE(std::same_as<decltype(wh::compose::graph_start_node_key),
                              const std::string_view>);
}
