#include <catch2/catch_test_macros.hpp>

#include "wh/compose/types.hpp"

TEST_CASE("compose types expose stable contract names and default origins",
          "[UT][wh/compose/types.hpp][to_string][condition][branch][boundary]") {
  REQUIRE(wh::compose::to_string(wh::compose::node_contract::value) == "value");
  REQUIRE(wh::compose::to_string(wh::compose::node_contract::stream) == "stream");
  REQUIRE(wh::compose::to_string(static_cast<wh::compose::node_contract>(99)) == "value");

  REQUIRE(wh::compose::default_exec_origin(wh::compose::node_kind::component) ==
          wh::compose::node_exec_origin::authored);
  REQUIRE(wh::compose::default_exec_origin(wh::compose::node_kind::lambda) ==
          wh::compose::node_exec_origin::authored);
  REQUIRE(wh::compose::default_exec_origin(wh::compose::node_kind::tools) ==
          wh::compose::node_exec_origin::authored);
  REQUIRE(wh::compose::default_exec_origin(wh::compose::node_kind::subgraph) ==
          wh::compose::node_exec_origin::lowered);
  REQUIRE(wh::compose::default_exec_origin(wh::compose::node_kind::passthrough) ==
          wh::compose::node_exec_origin::lowered);
}

TEST_CASE("compose types expose edge and branch data contracts",
          "[UT][wh/compose/types.hpp][graph_edge][condition][branch][boundary]") {
  wh::compose::graph_boundary boundary{};
  REQUIRE(boundary.input == wh::compose::node_contract::value);
  REQUIRE(boundary.output == wh::compose::node_contract::value);

  wh::compose::graph_edge edge{};
  edge.from = "a";
  edge.to = "b";
  edge.options.no_control = true;
  edge.options.limits.max_items = 4U;
  REQUIRE(edge.from == "a");
  REQUIRE(edge.to == "b");
  REQUIRE(edge.options.no_control);
  REQUIRE(edge.options.limits.max_items == 4U);
}

TEST_CASE("compose types expose default adapter branch and diagnostic carriers",
          "[UT][wh/compose/types.hpp][edge_adapter][condition][branch][boundary]") {
  STATIC_REQUIRE(std::same_as<wh::compose::graph_value_map::key_type, std::string>);

  wh::compose::edge_adapter adapter{};
  REQUIRE(adapter.kind == wh::compose::edge_adapter_kind::none);
  REQUIRE_FALSE(adapter.custom.to_stream.has_value());
  REQUIRE_FALSE(adapter.custom.to_value.has_value());

  wh::compose::graph_value_branch value_branch{};
  value_branch.from = "source";
  value_branch.end_nodes = {"left", "right"};
  REQUIRE(value_branch.from == "source");
  REQUIRE(value_branch.end_nodes.size() == 2U);
  REQUIRE_FALSE(static_cast<bool>(value_branch.selector_ids));

  wh::compose::graph_stream_branch stream_branch{};
  stream_branch.from = "source";
  stream_branch.end_nodes = {"stream-left"};
  REQUIRE(stream_branch.end_nodes.size() == 1U);
  REQUIRE_FALSE(static_cast<bool>(stream_branch.selector_ids));

  wh::compose::graph_diagnostic diagnostic{};
  diagnostic.code = wh::core::errc::timeout;
  diagnostic.message = "timed out";
  REQUIRE(diagnostic.code == wh::core::errc::timeout);
  REQUIRE(diagnostic.message == "timed out");
}
