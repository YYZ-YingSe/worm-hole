#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph/diff.hpp"
#include "wh/compose/node/passthrough.hpp"

TEST_CASE("graph diff facade rejects uncompiled graphs and diffs compiled snapshots",
          "[UT][wh/compose/graph/diff.hpp][diff_graph][condition][branch][boundary]") {
  wh::compose::graph baseline{};
  wh::compose::graph candidate{};
  auto invalid = wh::compose::diff_graph(baseline, candidate);
  REQUIRE(invalid.has_error());
  REQUIRE(invalid.error() == wh::core::errc::contract_violation);

  REQUIRE(baseline.add_passthrough(wh::compose::make_passthrough_node("worker"))
              .has_value());
  REQUIRE(baseline.add_entry_edge("worker").has_value());
  REQUIRE(baseline.add_exit_edge("worker").has_value());
  REQUIRE(baseline.compile().has_value());

  REQUIRE(candidate.add_passthrough(wh::compose::make_passthrough_node("worker"))
              .has_value());
  REQUIRE(candidate.add_entry_edge("worker").has_value());
  REQUIRE(candidate.add_exit_edge("worker").has_value());
  REQUIRE(candidate.compile().has_value());

  auto diff = wh::compose::diff_graph(baseline, candidate);
  REQUIRE(diff.has_value());
  REQUIRE(diff.value().entries.empty());
}

TEST_CASE("graph diff facade reports compile-visible changes between compiled graphs",
          "[UT][wh/compose/graph/diff.hpp][diff_graph][condition][branch][boundary]") {
  wh::compose::graph baseline{};
  wh::compose::graph candidate{};

  REQUIRE(baseline.add_passthrough(wh::compose::make_passthrough_node("worker"))
              .has_value());
  REQUIRE(baseline.add_entry_edge("worker").has_value());
  REQUIRE(baseline.add_exit_edge("worker").has_value());
  REQUIRE(baseline.compile().has_value());

  REQUIRE(candidate.add_passthrough(wh::compose::make_passthrough_node("other"))
              .has_value());
  REQUIRE(candidate.add_entry_edge("other").has_value());
  REQUIRE(candidate.add_exit_edge("other").has_value());
  REQUIRE(candidate.compile().has_value());

  auto diff = wh::compose::diff_graph(baseline, candidate);
  REQUIRE(diff.has_value());
  REQUIRE_FALSE(diff->entries.empty());
}
