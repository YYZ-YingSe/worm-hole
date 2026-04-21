#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph.hpp"
#include "wh/compose/graph/detail/graph_core.hpp"
#include "wh/compose/node/passthrough.hpp"

TEST_CASE("graph core indices plans and storage helpers preserve topology state across copy move "
          "and clear",
          "[UT][wh/compose/graph/detail/"
          "graph_core.hpp][graph_core::clear_cold_authoring_state][condition][branch][boundary]") {
  using graph_core = wh::compose::detail::graph_core;

  graph_core::indexed_value_branch_definition value_branch{
      .end_nodes_sorted = {1U, 3U, 5U},
  };
  REQUIRE(value_branch.contains(3U));
  REQUIRE_FALSE(value_branch.contains(4U));

  graph_core::indexed_stream_branch_definition stream_branch{
      .end_nodes_sorted = {2U, 4U},
  };
  REQUIRE(stream_branch.contains(4U));
  REQUIRE_FALSE(stream_branch.contains(1U));

  graph_core::csr_edge_index edges{};
  edges.offsets = {0U, 2U, 3U};
  edges.edge_ids = {7U, 8U, 9U};
  auto node_zero = edges.edge_ids_for(0U);
  REQUIRE(node_zero.size() == 2U);
  REQUIRE(node_zero[0] == 7U);

  graph_core::graph_index index{};
  index.incoming_control_edges = edges;
  index.incoming_data_edges = edges;
  index.outgoing_data_edges = edges;
  index.outgoing_control_edges = edges;
  index.has_value_branch_by_source = {1U};
  index.value_branch_index_by_source = {0U};
  index.value_branch_defs = {value_branch};
  index.has_stream_branch_by_source = {1U};
  index.stream_branch_index_by_source = {0U};
  index.stream_branch_defs = {stream_branch};
  REQUIRE(index.incoming_control(0U).size() == 2U);
  REQUIRE(index.incoming_data(0U).size() == 2U);
  REQUIRE(index.outgoing_data(0U).size() == 2U);
  REQUIRE(index.outgoing_control(0U).size() == 2U);
  REQUIRE(index.value_branch_for_source(0U) != nullptr);
  REQUIRE(index.stream_branch_for_source(0U) != nullptr);

  graph_core::control_graph_index control{};
  control.control_out_offsets = {0U, 2U, 3U};
  control.control_out_nodes = {1U, 2U, 3U};
  auto neighbors = control.out_neighbors(0U);
  REQUIRE(neighbors.size() == 2U);
  REQUIRE(neighbors[1] == 2U);

  graph_core core{};
  core.options_.name = "graph-core";
  core.nodes_.emplace("worker", wh::compose::make_passthrough_node("worker"));
  core.compiled_nodes_.push_back(wh::compose::compiled_node{
      .meta =
          wh::compose::compiled_node_meta{
              .key = "worker",
          },
  });
  core.node_insertion_order_.push_back("worker");
  core.node_id_index_.emplace("worker", 0U);
  core.edges_.push_back(wh::compose::graph_edge{
      .from = "start",
      .to = "worker",
  });
  core.value_branches_.emplace("worker", graph_core::value_branch_definition{
                                             .end_nodes = {"left", "right"},
                                         });
  core.stream_branches_.emplace("worker", graph_core::stream_branch_definition{
                                              .end_nodes = {"stream"},
                                          });
  core.compile_order_.push_back("worker");
  core.compiled_ = true;
  core.first_error_ = wh::core::make_error_code(wh::core::errc::invalid_argument);
  core.snapshot_cache_ = wh::compose::graph_snapshot{};

  graph_core copied{};
  copied.copy_from(core);
  REQUIRE(copied.options_.name == "graph-core");
  REQUIRE(copied.nodes_.contains("worker"));
  REQUIRE(copied.compiled_);
  REQUIRE(copied.first_error_.has_value());

  graph_core moved{};
  moved.move_from(core);
  REQUIRE(moved.options_.name == "graph-core");
  REQUIRE(moved.nodes_.contains("worker"));
  REQUIRE(moved.compiled_);
  REQUIRE(core.snapshot_once_.has_value());

  moved.clear_cold_authoring_state();
  REQUIRE(moved.nodes_.empty());
  REQUIRE(moved.edges_.empty());
  REQUIRE(moved.value_branches_.empty());
  REQUIRE(moved.stream_branches_.empty());
  REQUIRE(moved.node_insertion_order_.empty());
  REQUIRE(moved.node_id_index_.empty());
  REQUIRE(moved.compile_order_.empty());

  moved.reset_snapshot_state();
  REQUIRE_FALSE(moved.snapshot_cache_.has_value());
  REQUIRE(moved.snapshot_once_.has_value());
}

TEST_CASE("graph core helper indices return empty and null branches for missing sources",
          "[UT][wh/compose/graph/detail/"
          "graph_core.hpp][graph_core::graph_index::value_branch_for_source][condition][branch]["
          "boundary]") {
  using graph_core = wh::compose::detail::graph_core;

  graph_core::csr_edge_index empty_edges{};
  REQUIRE(empty_edges.edge_ids_for(0U).empty());

  graph_core::graph_index index{};
  index.has_value_branch_by_source = {0U};
  index.value_branch_index_by_source = {graph_core::graph_index::no_branch_index};
  index.has_stream_branch_by_source = {1U};
  index.stream_branch_index_by_source = {9U};

  REQUIRE(index.value_branch_for_source(0U) == nullptr);
  REQUIRE(index.value_branch_for_source(3U) == nullptr);
  REQUIRE(index.stream_branch_for_source(0U) == nullptr);
  REQUIRE(index.stream_branch_for_source(2U) == nullptr);

  graph_core::control_graph_index control{};
  control.control_out_offsets = {0U, 0U};
  REQUIRE(control.out_neighbors(0U).empty());
}
