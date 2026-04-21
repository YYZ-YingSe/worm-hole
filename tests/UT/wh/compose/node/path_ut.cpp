#include <array>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "wh/compose/node/path.hpp"

TEST_CASE("make_node_path builds node paths from initializer lists",
          "[UT][wh/compose/node/path.hpp][make_node_path][boundary]") {
  const auto path = wh::compose::make_node_path({"alpha", "beta", "gamma"});
  REQUIRE(path.to_string() == "alpha/beta/gamma");
}

TEST_CASE("make_node_path builds node paths from spans",
          "[UT][wh/compose/node/path.hpp][make_node_path][condition][boundary]") {
  constexpr std::array<std::string_view, 2U> segments{"root", "child"};
  const auto path = wh::compose::make_node_path(std::span<const std::string_view>{segments});
  REQUIRE(path.to_string() == "root/child");
}

TEST_CASE("parse_node_path accepts empty single and multi-segment inputs",
          "[UT][wh/compose/node/path.hpp][parse_node_path][condition][branch][boundary]") {
  const auto empty = wh::compose::parse_node_path("");
  REQUIRE(empty.has_value());
  REQUIRE(empty.value().empty());

  const auto single = wh::compose::parse_node_path("worker");
  REQUIRE(single.has_value());
  REQUIRE(single.value().to_string() == "worker");

  const auto nested = wh::compose::parse_node_path("root/child/leaf");
  REQUIRE(nested.has_value());
  REQUIRE(nested.value().to_string() == "root/child/leaf");
}

TEST_CASE("parse_node_path rejects empty path segments from malformed separators",
          "[UT][wh/compose/node/path.hpp][parse_node_path][condition][branch]") {
  const auto doubled = wh::compose::parse_node_path("root//child");
  REQUIRE(doubled.has_error());
  REQUIRE(doubled.error() == wh::core::errc::invalid_argument);

  const auto leading = wh::compose::parse_node_path("/root");
  REQUIRE(leading.has_error());
  REQUIRE(leading.error() == wh::core::errc::invalid_argument);

  const auto trailing = wh::compose::parse_node_path("root/");
  REQUIRE(trailing.has_error());
  REQUIRE(trailing.error() == wh::core::errc::invalid_argument);
}
