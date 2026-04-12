#include <catch2/catch_test_macros.hpp>

#include <tuple>

#include <stdexec/execution.hpp>

#include "wh/compose/graph/graph.hpp"
#include "wh/compose/node/passthrough.hpp"

TEST_CASE("graph class compiles passthrough nodes and exposes snapshots",
          "[UT][wh/compose/graph/graph.hpp][graph::invoke][condition][branch][boundary]") {
  wh::compose::graph graph{};
  REQUIRE_FALSE(graph.compiled());
  REQUIRE(graph.options().name == "graph");
  REQUIRE(graph.compile_order().empty());
  REQUIRE(graph.diagnostics().empty());
  REQUIRE(graph.node_id("missing").has_error());
  REQUIRE(graph.add_passthrough(wh::compose::make_passthrough_node("worker"))
              .has_value());
  REQUIRE(graph.node_id("worker").has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());
  REQUIRE(graph.compiled());
  REQUIRE(graph.boundary().input == wh::compose::node_contract::value);
  REQUIRE(graph.snapshot_view().nodes.size() == 1U);
  REQUIRE(graph.restore_shape().nodes.size() == 1U);

  wh::compose::graph_invoke_request request{};
  request.input = wh::compose::graph_value{9};
  wh::core::run_context context{};
  auto awaited = stdexec::sync_wait(graph.invoke(context, std::move(request)));
  REQUIRE(awaited.has_value());
  REQUIRE(std::get<0>(*awaited).has_value());
  REQUIRE(*wh::core::any_cast<int>(&std::get<0>(*awaited).value().output_status.value()) ==
          9);
}

TEST_CASE("graph facade exposes boundary snapshots copy semantics and compile option constructors",
          "[UT][wh/compose/graph/graph.hpp][graph::compile_options_snapshot][branch][boundary]") {
  wh::compose::graph_compile_options options{};
  options.name = "g2";
  options.boundary = {.input = wh::compose::node_contract::stream,
                      .output = wh::compose::node_contract::stream};

  wh::compose::graph graph{options};
  REQUIRE(graph.options().name == "g2");
  REQUIRE(graph.boundary().input == wh::compose::node_contract::stream);
  REQUIRE(graph.boundary().output == wh::compose::node_contract::stream);
  REQUIRE(graph.compile_options_snapshot().name == "g2");

  wh::compose::graph copied{graph};
  REQUIRE(copied.options().name == "g2");
  REQUIRE_FALSE(copied.compiled());
}
