#include <catch2/catch_test_macros.hpp>

#include "wh/compose/authored/stream_branch.hpp"
#include "wh/compose/node/passthrough.hpp"

TEST_CASE("stream branch validates targets and selector configuration",
          "[UT][wh/compose/authored/stream_branch.hpp][stream_branch::add_target][condition][branch][boundary]") {
  wh::compose::stream_branch branch{};
  REQUIRE(branch.add_target("").has_error());
  REQUIRE(branch.add_target("left").has_value());
  REQUIRE(branch.add_target("right").has_value());
  REQUIRE(branch.targets() == std::vector<std::string>{"left", "right"});
  REQUIRE(branch.end_nodes() == std::vector<std::string>{"left", "right"});

  REQUIRE(branch
              .set_selector([](wh::compose::graph_stream_reader,
                               wh::core::run_context &)
                                -> wh::core::result<std::vector<std::string>> {
                return std::vector<std::string>{"left"};
              })
              .has_value());
  REQUIRE(static_cast<bool>(branch.selector()));
}

TEST_CASE("stream branch lowers valid targets and rejects duplicates",
          "[UT][wh/compose/authored/stream_branch.hpp][stream_branch::apply][condition][branch][boundary]") {
  wh::compose::graph graph{};
  REQUIRE(graph.add_passthrough(
              wh::compose::make_passthrough_node<wh::compose::node_contract::stream>(
                  "source"))
              .has_value());
  REQUIRE(graph.add_passthrough(
              wh::compose::make_passthrough_node<wh::compose::node_contract::stream>(
                  "left"))
              .has_value());
  REQUIRE(graph.add_passthrough(
              wh::compose::make_passthrough_node<wh::compose::node_contract::stream>(
                  "right"))
              .has_value());

  wh::compose::stream_branch branch{};
  REQUIRE(branch.add_target("left").has_value());
  REQUIRE(branch.add_target("right").has_value());
  REQUIRE(branch.apply(graph, "source").has_value());

  wh::compose::stream_branch duplicate{};
  REQUIRE(duplicate.add_target("left").has_value());
  REQUIRE(duplicate.add_target("left").has_value());
  auto applied = duplicate.apply(graph, "source");
  REQUIRE(applied.has_error());
  REQUIRE(applied.error() == wh::core::errc::already_exists);
}

TEST_CASE("stream branch rejects empty selector objects and empty target sets on apply",
          "[UT][wh/compose/authored/stream_branch.hpp][stream_branch::set_selector][condition][branch][boundary]") {
  wh::compose::stream_branch branch{};
  auto invalid_selector =
      branch.set_selector(wh::compose::stream_branch_selector{nullptr});
  REQUIRE(invalid_selector.has_error());
  REQUIRE(invalid_selector.error() == wh::core::errc::invalid_argument);

  wh::compose::graph graph{};
  REQUIRE(graph.add_passthrough(
              wh::compose::make_passthrough_node<wh::compose::node_contract::stream>(
                  "source"))
              .has_value());
  auto missing_targets = branch.apply(graph, "source");
  REQUIRE(missing_targets.has_error());
  REQUIRE(missing_targets.error() == wh::core::errc::invalid_argument);
}
