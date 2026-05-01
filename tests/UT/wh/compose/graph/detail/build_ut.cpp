#include <catch2/catch_test_macros.hpp>

#include "helper/agent_authoring_support.hpp"
#include "wh/compose/graph.hpp"
#include "wh/compose/graph/detail/build.hpp"
#include "wh/compose/node.hpp"
#include "wh/compose/node/passthrough.hpp"

TEST_CASE(
    "build helpers resolve required tools endpoints and graph adders expose convenience overloads",
    "[UT][wh/compose/graph/detail/"
    "build.hpp][validate_tools_node_entrypoints][condition][branch][boundary]") {
  REQUIRE(wh::compose::detail::tools_node_required_endpoint(
              wh::compose::node_exec_mode::sync, wh::compose::node_contract::value) == "invoke");
  REQUIRE(wh::compose::detail::tools_node_required_endpoint(wh::compose::node_exec_mode::async,
                                                            wh::compose::node_contract::value) ==
          "async_invoke");
  REQUIRE(wh::compose::detail::tools_node_required_endpoint(
              wh::compose::node_exec_mode::sync, wh::compose::node_contract::stream) == "stream");
  REQUIRE(wh::compose::detail::tools_node_required_endpoint(wh::compose::node_exec_mode::async,
                                                            wh::compose::node_contract::stream) ==
          "async_stream");

  wh::compose::tool_entry invoke_entry{};
  invoke_entry.invoke = [](const wh::compose::tool_call &call,
                           wh::tool::call_scope) -> wh::core::result<wh::compose::graph_value> {
    return wh::compose::graph_value{call.arguments};
  };
  REQUIRE(wh::compose::detail::tools_entry_matches_endpoint(
      invoke_entry, wh::compose::node_exec_mode::sync, wh::compose::node_contract::value));
  REQUIRE_FALSE(wh::compose::detail::tools_entry_matches_endpoint(
      invoke_entry, wh::compose::node_exec_mode::async, wh::compose::node_contract::value));

  wh::compose::node_descriptor descriptor{
      .key = "tools",
      .kind = wh::compose::node_kind::tools,
      .exec_mode = wh::compose::node_exec_mode::sync,
      .output_contract = wh::compose::node_contract::value,
  };
  wh::compose::tools_payload payload{};
  payload.registry.emplace("invoke", invoke_entry);
  payload.runtime_options.sequential = false;
  auto non_sequential =
      wh::compose::detail::validate_tools_node_entrypoints("tools", descriptor, payload);
  REQUIRE(non_sequential.has_value());
  REQUIRE(non_sequential->find("non-sequential sync execution") != std::string::npos);

  descriptor.exec_mode = wh::compose::node_exec_mode::async;
  descriptor.output_contract = wh::compose::node_contract::stream;
  payload.runtime_options.sequential = true;
  auto missing_async =
      wh::compose::detail::validate_tools_node_entrypoints("tools", descriptor, payload);
  REQUIRE(missing_async.has_value());
  REQUIRE(missing_async->find("async_stream") != std::string::npos);

  payload.registry.clear();
  payload.runtime_options.missing = invoke_entry;
  auto invalid_missing =
      wh::compose::detail::validate_tools_node_entrypoints("tools", descriptor, payload);
  REQUIRE(invalid_missing.has_value());
  REQUIRE(invalid_missing->find("missing-tool entry") != std::string::npos);

  wh::compose::graph lambda_graph{};
  REQUIRE(lambda_graph
              .add_lambda(
                  "worker",
                  [](wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> { return std::move(input); })
              .has_value());
  REQUIRE(lambda_graph.node_id("worker").has_value());

  wh::compose::graph passthrough_graph{};
  REQUIRE(passthrough_graph.add_passthrough("pass").has_value());
  REQUIRE(passthrough_graph.node_id("pass").has_value());

  wh::compose::graph component_graph{};
  REQUIRE(component_graph
              .add_component<wh::compose::component_kind::model, wh::compose::node_contract::value,
                             wh::compose::node_contract::stream, wh::compose::node_exec_mode::sync>(
                  "model", wh::testing::helper::sync_probe_model{})
              .has_value());
  REQUIRE(component_graph.node_id("model").has_value());

  auto child = wh::testing::helper::make_passthrough_graph("inner");
  REQUIRE(child.has_value());
  wh::compose::graph subgraph_graph{};
  REQUIRE(subgraph_graph.add_subgraph("sub", child.value()).has_value());
  REQUIRE(subgraph_graph.node_id("sub").has_value());

  wh::compose::tool_registry registry{};
  registry.emplace("echo", invoke_entry);
  wh::compose::graph tools_graph{};
  REQUIRE(tools_graph
              .add_tools<wh::compose::node_contract::value, wh::compose::node_contract::value,
                         wh::compose::node_exec_mode::sync>("tools", registry, {},
                                                            wh::compose::tools_options{})
              .has_value());
  REQUIRE(tools_graph.node_id("tools").has_value());
}

TEST_CASE("build helpers reject duplicate nodes missing edge endpoints and invalid edge flags",
          "[UT][wh/compose/graph/detail/build.hpp][graph::add_edge][condition][branch][boundary]") {
  wh::compose::graph duplicate_graph{};
  REQUIRE(duplicate_graph.add_passthrough("worker").has_value());

  auto duplicate = duplicate_graph.add_passthrough("worker");
  REQUIRE(duplicate.has_error());
  REQUIRE(duplicate.error() == wh::core::errc::already_exists);

  auto sticky = duplicate_graph.add_edge("worker", "missing");
  REQUIRE(sticky.has_error());
  REQUIRE(sticky.error() == wh::core::errc::already_exists);

  wh::compose::graph missing_graph{};
  REQUIRE(missing_graph.add_passthrough("worker").has_value());
  auto missing_endpoint = missing_graph.add_edge("worker", "missing");
  REQUIRE(missing_endpoint.has_error());
  REQUIRE(missing_endpoint.error() == wh::core::errc::not_found);

  wh::compose::graph invalid_graph{};
  REQUIRE(invalid_graph.add_passthrough("worker").has_value());
  wh::compose::edge_options invalid_flags{};
  invalid_flags.no_control = true;
  invalid_flags.no_data = true;
  auto invalid_edge = invalid_graph.add_edge("worker", "worker", invalid_flags);
  REQUIRE(invalid_edge.has_error());
  REQUIRE(invalid_edge.error() == wh::core::errc::invalid_argument);

  wh::compose::graph edge_graph{};
  REQUIRE(edge_graph.add_passthrough("worker").has_value());
  REQUIRE(edge_graph.add_entry_edge("worker").has_value());
  auto duplicate_edge = edge_graph.add_entry_edge("worker");
  REQUIRE(duplicate_edge.has_error());
  REQUIRE(duplicate_edge.error() == wh::core::errc::already_exists);
}
