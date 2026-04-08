#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph/restore_validation.hpp"
#include "wh/compose/node/passthrough.hpp"

TEST_CASE("restore validation requires compiled graph and validates matching shape",
          "[UT][wh/compose/graph/restore_validation.hpp][validate_restore][condition][branch][boundary]") {
  wh::compose::graph graph{};
  wh::compose::checkpoint_state checkpoint{};

  auto uncompiled = wh::compose::validate_restore(graph, checkpoint);
  REQUIRE(uncompiled.has_value());
  REQUIRE_FALSE(uncompiled.value().restorable);

  REQUIRE(graph.add_passthrough(wh::compose::make_passthrough_node("worker"))
              .has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  checkpoint.restore_shape = graph.restore_shape();
  auto compiled = wh::compose::validate_restore(graph, checkpoint);
  REQUIRE(compiled.has_value());
  REQUIRE(compiled.value().restorable);
}

TEST_CASE("restore validation aliases restore-check result types through the public header",
          "[UT][wh/compose/graph/restore_validation.hpp][restore_validation_result][condition][branch][boundary]") {
  wh::compose::restore_validation_result result{};
  result.restorable = false;
  result.diff.entries.push_back(
      {.kind = wh::compose::restore_diff_kind::graph_option});
  result.issues.push_back(
      {.kind = wh::compose::restore_issue_kind::graph_changed,
       .subject = "graph",
       .message = "changed"});

  REQUIRE_FALSE(result.restorable);
  REQUIRE(result.diff.contains(wh::compose::restore_diff_kind::graph_option));
  REQUIRE(result.issues.front().kind == wh::compose::restore_issue_kind::graph_changed);
}
