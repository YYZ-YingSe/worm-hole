#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph/edge_activity.hpp"

TEST_CASE("edge activity classifier partitions edges by selector status",
          "[UT][wh/compose/graph/edge_activity.hpp][classify_edges][condition][branch][boundary]") {
  std::array<wh::compose::graph_edge, 3> edges{{
      {.from = "a", .to = "b"},
      {.from = "b", .to = "c"},
      {.from = "c", .to = "d"},
  }};

  auto sets = wh::compose::classify_edges(std::span<const wh::compose::graph_edge>{edges},
                                          [](const wh::compose::graph_edge &edge) {
                                            if (edge.from == "a") {
                                              return wh::compose::edge_activity::active;
                                            }
                                            if (edge.from == "b") {
                                              return wh::compose::edge_activity::waiting;
                                            }
                                            return wh::compose::edge_activity::disabled;
                                          });

  REQUIRE(sets.active.size() == 1U);
  REQUIRE(sets.waiting.size() == 1U);
  REQUIRE(sets.disabled.size() == 1U);
}

TEST_CASE(
    "edge activity classifier preserves input order inside each partition and handles empty spans",
    "[UT][wh/compose/graph/edge_activity.hpp][classify_edges][condition][branch][boundary]") {
  const std::array<wh::compose::graph_edge, 0> empty_edges{};
  auto empty_sets = wh::compose::classify_edges(
      std::span<const wh::compose::graph_edge>{empty_edges},
      [](const wh::compose::graph_edge &) { return wh::compose::edge_activity::active; });
  REQUIRE(empty_sets.active.empty());
  REQUIRE(empty_sets.waiting.empty());
  REQUIRE(empty_sets.disabled.empty());

  std::array<wh::compose::graph_edge, 3> edges{{
      {.from = "x", .to = "a"},
      {.from = "y", .to = "b"},
      {.from = "z", .to = "c"},
  }};
  auto sets = wh::compose::classify_edges(
      std::span<const wh::compose::graph_edge>{edges}, [](const wh::compose::graph_edge &edge) {
        return edge.to == "b" ? wh::compose::edge_activity::waiting
                              : wh::compose::edge_activity::active;
      });
  REQUIRE(sets.active.size() == 2U);
  REQUIRE(sets.active[0].from == "x");
  REQUIRE(sets.active[1].from == "z");
  REQUIRE(sets.waiting.size() == 1U);
  REQUIRE(sets.waiting[0].from == "y");
}
