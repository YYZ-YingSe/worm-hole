#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph/compile_info.hpp"

TEST_CASE("graph compile info metadata reports active state handlers and node fields",
          "[UT][wh/compose/graph/compile_info.hpp][graph_compile_state_handler_metadata::any][condition][branch][boundary]") {
  wh::compose::graph_compile_state_handler_metadata handlers{};
  REQUIRE_FALSE(handlers.any());
  handlers.pre = true;
  REQUIRE(handlers.any());
  handlers = {.stream_post = true};
  REQUIRE(handlers.any());

  wh::compose::graph_compile_info info{};
  info.name = "g";
  info.compile_order = {"a", "b"};
  info.node_key_to_id.emplace("a", 1U);
  info.nodes.push_back(wh::compose::graph_compile_node_info{
      .key = "a",
      .node_id = 1U,
      .has_sender = true,
  });
  REQUIRE(info.name == "g");
  REQUIRE(info.compile_order.size() == 2U);
  REQUIRE(info.node_key_to_id.at("a") == 1U);
  REQUIRE(info.nodes.front().has_sender);
}

TEST_CASE("graph compile info stores node options branches and nested subgraph snapshots",
          "[UT][wh/compose/graph/compile_info.hpp][graph_compile_info][branch][boundary]") {
  wh::compose::graph_compile_info info{};
  info.mode = wh::compose::graph_runtime_mode::pregel;
  info.dispatch_policy = wh::compose::graph_dispatch_policy::next_wave;
  info.branches.push_back({.from = "branch",
                           .end_nodes = {"left", "right"}});
  info.subgraphs.emplace("branch", wh::compose::graph_compile_info{.name = "sub"});
  info.nodes.push_back(wh::compose::graph_compile_node_info{
      .key = "n1",
      .node_id = 2U,
      .has_subgraph = true,
      .field_mapping = {.input_key = "in", .output_key = "out"},
      .options = {.name = "Node",
                  .type = "lambda",
                  .allow_no_control = true,
                  .sync_dispatch = wh::compose::sync_dispatch::inline_control,
                  .state_handlers = {.post = true}},
  });

  REQUIRE(info.mode == wh::compose::graph_runtime_mode::pregel);
  REQUIRE(info.dispatch_policy == wh::compose::graph_dispatch_policy::next_wave);
  REQUIRE(info.branches.size() == 1U);
  REQUIRE(info.branches.front().end_nodes ==
          std::vector<std::string>({"left", "right"}));
  REQUIRE(info.subgraphs.at("branch").name == "sub");
  REQUIRE(info.nodes.front().has_subgraph);
  REQUIRE(info.nodes.front().field_mapping.input_key == "in");
  REQUIRE(info.nodes.front().options.name == "Node");
  REQUIRE(info.nodes.front().options.sync_dispatch ==
          wh::compose::sync_dispatch::inline_control);
  REQUIRE(info.nodes.front().options.state_handlers.post);
}
