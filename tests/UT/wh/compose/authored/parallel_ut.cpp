#include <catch2/catch_test_macros.hpp>

#include "wh/compose/authored/parallel.hpp"
#include "wh/compose/node/lambda.hpp"
#include "wh/compose/node/passthrough.hpp"
#include "wh/compose/node/subgraph.hpp"
#include "wh/compose/node/tools_builder.hpp"
#include "wh/model/echo_chat_model.hpp"

namespace {

[[nodiscard]] auto make_subgraph() -> wh::compose::graph {
  wh::compose::graph graph{};
  REQUIRE(graph.add_passthrough(wh::compose::make_passthrough_node("inner")).has_value());
  REQUIRE(graph.add_entry_edge("inner").has_value());
  REQUIRE(graph.add_exit_edge("inner").has_value());
  REQUIRE(graph.compile().has_value());
  return graph;
}

} // namespace

TEST_CASE("parallel builder accepts authored node kinds and exposes stored nodes",
          "[UT][wh/compose/authored/"
          "parallel.hpp][parallel::add_component][condition][branch][boundary]") {
  wh::compose::parallel group{};

  const auto component = wh::compose::make_component_node<wh::compose::component_kind::model,
                                                          wh::compose::node_contract::value,
                                                          wh::compose::node_contract::value>(
      "model", wh::model::echo_chat_model{});
  REQUIRE(group.add_component(component).has_value());

  REQUIRE(group
              .add_lambda(wh::compose::make_lambda_node(
                  "lambda",
                  [](wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> { return std::move(input); }))
              .has_value());

  const auto subgraph = wh::compose::make_subgraph_node("sub", make_subgraph());
  REQUIRE(group.add_subgraph(subgraph).has_value());

  REQUIRE(group.add_tools(wh::compose::make_tools_node("tools", wh::compose::tool_registry{}))
              .has_value());

  REQUIRE(group.add_passthrough(wh::compose::make_passthrough_node("pass")).has_value());
  REQUIRE(group.nodes().size() == 5U);
}

TEST_CASE("parallel apply requires at least two nodes and returns tail keys",
          "[UT][wh/compose/authored/parallel.hpp][parallel::apply][condition][branch][boundary]") {
  wh::compose::parallel too_small{};
  REQUIRE(too_small.add_passthrough(wh::compose::make_passthrough_node("only")).has_value());
  wh::compose::graph graph{};
  REQUIRE(graph.add_passthrough(wh::compose::make_passthrough_node("source")).has_value());
  auto invalid = too_small.apply(graph, "source");
  REQUIRE(invalid.has_error());
  REQUIRE(invalid.error() == wh::core::errc::invalid_argument);

  wh::compose::parallel valid{};
  REQUIRE(valid.add_passthrough(wh::compose::make_passthrough_node("left")).has_value());
  REQUIRE(valid.add_passthrough(wh::compose::make_passthrough_node("right")).has_value());
  auto applied = std::move(valid).apply(graph, "source");
  REQUIRE(applied.has_value());
  REQUIRE(applied.value() == std::vector<std::string>{"left", "right"});
}

TEST_CASE("parallel builder rejects authored nodes with empty keys",
          "[UT][wh/compose/authored/"
          "parallel.hpp][parallel::add_passthrough][condition][branch][boundary]") {
  wh::compose::parallel group{};

  auto invalid = group.add_passthrough(wh::compose::make_passthrough_node(""));
  REQUIRE(invalid.has_error());
  REQUIRE(invalid.error() == wh::core::errc::invalid_argument);
}
