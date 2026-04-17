#include <catch2/catch_test_macros.hpp>

#include <tuple>

#include <stdexec/execution.hpp>

#include "wh/compose/graph/graph.hpp"
#include "wh/compose/node/passthrough.hpp"

TEST_CASE("passthrough node factories expose stable contracts for value and stream nodes",
          "[UT][wh/compose/node/passthrough.hpp][make_passthrough_node][condition][branch][boundary]") {
  auto value_node = wh::compose::make_passthrough_node("value");
  REQUIRE(value_node.key() == "value");
  REQUIRE(value_node.input_contract() == wh::compose::node_contract::value);
  REQUIRE(value_node.output_contract() == wh::compose::node_contract::value);
  REQUIRE(value_node.input_gate() == wh::compose::input_gate::open());
  REQUIRE(value_node.output_gate() == wh::compose::output_gate::passthrough());
  REQUIRE(value_node.exec_mode() == wh::compose::node_exec_mode::sync);
  REQUIRE(value_node.exec_origin() == wh::compose::node_exec_origin::lowered);
  REQUIRE(value_node.options().name == "value");

  auto stream_node =
      wh::compose::make_passthrough_node<wh::compose::node_contract::stream>(
          "stream");
  REQUIRE(stream_node.input_contract() == wh::compose::node_contract::stream);
  REQUIRE(stream_node.output_contract() == wh::compose::node_contract::stream);
  REQUIRE(stream_node.input_gate() == wh::compose::input_gate::reader());
  REQUIRE(stream_node.output_gate() == wh::compose::output_gate::reader());
}

TEST_CASE("passthrough node graph execution forwards payload unchanged",
          "[UT][wh/compose/node/passthrough.hpp][passthrough_node::compile][condition][branch]") {
  wh::compose::graph graph{};
  REQUIRE(graph.add_passthrough(wh::compose::make_passthrough_node("worker"))
              .has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  wh::compose::graph_invoke_request request{};
  request.input = wh::compose::graph_input::value(19);
  wh::core::run_context context{};
  auto awaited = stdexec::sync_wait(graph.invoke(context, std::move(request)));
  REQUIRE(awaited.has_value());
  REQUIRE(std::get<0>(*awaited).has_value());
  REQUIRE(std::get<0>(*awaited).value().output_status.has_value());
  REQUIRE(*wh::core::any_cast<int>(
              &std::get<0>(*awaited).value().output_status.value()) == 19);
}

TEST_CASE("stream passthrough node preserves reader payloads through graph invoke",
          "[UT][wh/compose/node/passthrough.hpp][passthrough_node::compile][condition][boundary]") {
  wh::compose::graph graph{};
  REQUIRE(graph
              .add_passthrough<wh::compose::node_contract::stream>("worker")
              .has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  auto input_reader = wh::compose::make_values_stream_reader(
      std::vector<wh::compose::graph_value>{wh::compose::graph_value{1},
                                            wh::compose::graph_value{2}});
  REQUIRE(input_reader.has_value());

  wh::compose::graph_invoke_request request{};
  request.input =
      wh::compose::graph_input::value(std::move(input_reader).value());
  wh::core::run_context context{};
  auto awaited = stdexec::sync_wait(graph.invoke(context, std::move(request)));
  REQUIRE(awaited.has_value());
  REQUIRE(std::get<0>(*awaited).has_value());
  REQUIRE(std::get<0>(*awaited).value().output_status.has_error());
  REQUIRE(std::get<0>(*awaited).value().output_status.error() ==
          wh::core::errc::contract_violation);
}

TEST_CASE("stream passthrough node collects stream into value output at default graph boundary",
          "[UT][wh/compose/node/passthrough.hpp][passthrough_node::compile][branch][boundary]") {
  wh::compose::graph graph{
      wh::compose::graph_boundary{
          .input = wh::compose::node_contract::stream,
          .output = wh::compose::node_contract::value,
      }};
  REQUIRE(graph
              .add_passthrough<wh::compose::node_contract::stream>("worker")
              .has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  auto input_reader = wh::compose::make_values_stream_reader(
      std::vector<wh::compose::graph_value>{wh::compose::graph_value{1},
                                            wh::compose::graph_value{2}});
  REQUIRE(input_reader.has_value());

  wh::compose::graph_invoke_request request{};
  request.input =
      wh::compose::graph_input::stream(std::move(input_reader).value());
  wh::core::run_context context{};
  auto awaited = stdexec::sync_wait(graph.invoke(context, std::move(request)));
  REQUIRE(awaited.has_value());
  REQUIRE(std::get<0>(*awaited).has_value());
  REQUIRE(std::get<0>(*awaited).value().output_status.has_value());

  auto *collected = wh::core::any_cast<std::vector<wh::compose::graph_value>>(
      &std::get<0>(*awaited).value().output_status.value());
  REQUIRE(collected != nullptr);
  REQUIRE(collected->size() == 2U);
  REQUIRE(*wh::core::any_cast<int>(&(*collected)[0]) == 1);
  REQUIRE(*wh::core::any_cast<int>(&(*collected)[1]) == 2);
}

TEST_CASE("stream passthrough node preserves reader payloads at stream graph boundary",
          "[UT][wh/compose/node/passthrough.hpp][passthrough_node::compile][condition][branch][boundary]") {
  wh::compose::graph graph{
      wh::compose::graph_boundary{
          .input = wh::compose::node_contract::stream,
          .output = wh::compose::node_contract::stream,
      }};
  REQUIRE(graph
              .add_passthrough<wh::compose::node_contract::stream>("worker")
              .has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  auto input_reader = wh::compose::make_values_stream_reader(
      std::vector<wh::compose::graph_value>{wh::compose::graph_value{1},
                                            wh::compose::graph_value{2}});
  REQUIRE(input_reader.has_value());

  wh::compose::graph_invoke_request request{};
  request.input =
      wh::compose::graph_input::stream(std::move(input_reader).value());
  wh::core::run_context context{};
  auto awaited = stdexec::sync_wait(graph.invoke(context, std::move(request)));
  REQUIRE(awaited.has_value());
  REQUIRE(std::get<0>(*awaited).has_value());
  REQUIRE(std::get<0>(*awaited).value().output_status.has_value());

  auto *reader = wh::core::any_cast<wh::compose::graph_stream_reader>(
      &std::get<0>(*awaited).value().output_status.value());
  REQUIRE(reader != nullptr);
  auto collected = wh::compose::collect_graph_stream_reader(std::move(*reader));
  REQUIRE(collected.has_value());
  REQUIRE(collected.value().size() == 2U);
  REQUIRE(*wh::core::any_cast<int>(&collected.value()[0]) == 1);
  REQUIRE(*wh::core::any_cast<int>(&collected.value()[1]) == 2);
}
