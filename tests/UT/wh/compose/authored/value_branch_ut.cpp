#include <catch2/catch_test_macros.hpp>

#include "wh/compose/authored/value_branch.hpp"
#include "wh/compose/node/passthrough.hpp"

TEST_CASE("value branch validates cases and lowers to graph edges",
          "[UT][wh/compose/authored/value_branch.hpp][value_branch::add_case][condition][branch][boundary]") {
  wh::compose::value_branch branch{};
  REQUIRE(branch.add_case(wh::compose::value_branch_case{}).has_error());
  REQUIRE(branch.add_case("left",
                          [](const wh::compose::graph_value &,
                             wh::core::run_context &) -> wh::core::result<bool> {
                            return true;
                          })
              .has_value());
  REQUIRE(branch.add_case("right",
                          [](const wh::compose::graph_value &,
                             wh::core::run_context &) -> wh::core::result<bool> {
                            return false;
                          })
              .has_value());
  REQUIRE(branch.end_nodes() == std::vector<std::string>{"left", "right"});
  REQUIRE(branch.cases().size() == 2U);

  wh::compose::graph graph{};
  REQUIRE(graph.add_passthrough(wh::compose::make_passthrough_node("source"))
              .has_value());
  REQUIRE(graph.add_passthrough(wh::compose::make_passthrough_node("left"))
              .has_value());
  REQUIRE(graph.add_passthrough(wh::compose::make_passthrough_node("right"))
              .has_value());
  REQUIRE(branch.apply(graph, "source").has_value());
}

TEST_CASE("value branch rejects empty and duplicate targets during apply",
          "[UT][wh/compose/authored/value_branch.hpp][value_branch::apply][condition][branch][boundary]") {
  wh::compose::value_branch empty{};
  wh::compose::graph graph{};
  REQUIRE(graph.add_passthrough(wh::compose::make_passthrough_node("source"))
              .has_value());
  REQUIRE(empty.apply(graph, "source").has_error());

  wh::compose::value_branch duplicate{};
  REQUIRE(duplicate.add_case("same",
                             [](const wh::compose::graph_value &,
                                wh::core::run_context &) -> wh::core::result<bool> {
                               return true;
                             })
              .has_value());
  REQUIRE(duplicate.add_case("same",
                             [](const wh::compose::graph_value &,
                                wh::core::run_context &) -> wh::core::result<bool> {
                               return false;
                             })
              .has_value());
  REQUIRE(graph.add_passthrough(wh::compose::make_passthrough_node("same"))
              .has_value());
  auto applied = std::move(duplicate).apply(graph, "source");
  REQUIRE(applied.has_error());
  REQUIRE(applied.error() == wh::core::errc::already_exists);
}

TEST_CASE("value branch rejects cases without a target or predicate",
          "[UT][wh/compose/authored/value_branch.hpp][value_branch::add_case][condition][branch][boundary]") {
  wh::compose::value_branch branch{};

  auto missing_target = branch.add_case(
      "",
      [](const wh::compose::graph_value &, wh::core::run_context &)
          -> wh::core::result<bool> { return true; });
  REQUIRE(missing_target.has_error());
  REQUIRE(missing_target.error() == wh::core::errc::invalid_argument);

  auto missing_predicate =
      branch.add_case(wh::compose::value_branch_case{.to = "left"});
  REQUIRE(missing_predicate.has_error());
  REQUIRE(missing_predicate.error() == wh::core::errc::invalid_argument);
}
