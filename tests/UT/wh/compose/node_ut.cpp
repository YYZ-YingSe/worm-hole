#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "wh/compose/node.hpp"

static_assert(std::is_default_constructible_v<wh::compose::passthrough_node>);

TEST_CASE("compose node facade exports node-path helpers",
          "[UT][wh/compose/node.hpp][make_node_path][condition][branch][boundary]") {
  const auto path = wh::compose::make_node_path({"a", "b"});
  const auto parsed = wh::compose::parse_node_path("root/leaf");

  REQUIRE(path.to_string() == "a/b");
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->to_string() == "root/leaf");
}

TEST_CASE("compose node facade exports passthrough node builders through the public header",
          "[UT][wh/compose/node.hpp][make_passthrough_node][condition][branch][boundary]") {
  auto node = wh::compose::make_passthrough_node("pass");

  REQUIRE(node.descriptor().key == "pass");
  REQUIRE(node.descriptor().kind == wh::compose::node_kind::passthrough);
}
