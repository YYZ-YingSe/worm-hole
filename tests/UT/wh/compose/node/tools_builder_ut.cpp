#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <exec/task.hpp>
#include <stdexec/execution.hpp>

#include "helper/static_thread_scheduler.hpp"
#include "wh/compose/graph.hpp"
#include "wh/compose/graph/detail/build.hpp"
#include "wh/compose/graph/detail/compile.hpp"
#include "wh/compose/graph/detail/graph_class.hpp"
#include "wh/compose/graph/detail/invoke.hpp"
#include "wh/compose/node.hpp"
#include "wh/compose/node/tools_builder.hpp"

namespace {

[[nodiscard]] auto make_tool_batch(std::initializer_list<wh::compose::tool_call> calls)
    -> wh::compose::tool_batch {
  return wh::compose::tool_batch{
      .calls = std::vector<wh::compose::tool_call>{calls},
  };
}

[[nodiscard]] auto collect_tool_results(const wh::compose::graph_value &value)
    -> wh::core::result<std::reference_wrapper<const std::vector<wh::compose::tool_result>>> {
  if (const auto *typed = wh::core::any_cast<std::vector<wh::compose::tool_result>>(&value);
      typed != nullptr) {
    return std::cref(*typed);
  }
  return wh::core::result<std::reference_wrapper<const std::vector<wh::compose::tool_result>>>::
      failure(wh::core::errc::type_mismatch);
}

template <typename value_t>
[[nodiscard]] auto read_any(wh::compose::graph_value &&value) -> wh::core::result<value_t> {
  if (auto *typed = wh::core::any_cast<value_t>(&value); typed != nullptr) {
    return std::move(*typed);
  }
  return wh::core::result<value_t>::failure(wh::core::errc::type_mismatch);
}

[[nodiscard]] auto collect_tool_events(wh::compose::graph_stream_reader reader)
    -> wh::core::result<std::vector<wh::compose::tool_event>> {
  auto chunks = wh::compose::collect_graph_stream_reader(std::move(reader));
  if (chunks.has_error()) {
    return wh::core::result<std::vector<wh::compose::tool_event>>::failure(chunks.error());
  }
  std::vector<wh::compose::tool_event> events{};
  for (auto &chunk : chunks.value()) {
    auto *typed = wh::core::any_cast<wh::compose::tool_event>(&chunk);
    if (typed == nullptr) {
      return wh::core::result<std::vector<wh::compose::tool_event>>::failure(
          wh::core::errc::type_mismatch);
    }
    events.push_back(std::move(*typed));
  }
  return events;
}

[[nodiscard]] auto invoke_single_node_graph(wh::compose::graph &graph,
                                            wh::compose::graph_value input)
    -> wh::core::result<wh::compose::graph_invoke_result> {
  wh::compose::graph_invoke_request request{};
  request.input = wh::compose::graph_input::value(std::move(input));
  wh::core::run_context context{};
  auto awaited = stdexec::sync_wait(graph.invoke(context, std::move(request)));
  REQUIRE(awaited.has_value());
  return std::get<0>(std::move(*awaited));
}

} // namespace

TEST_CASE("tools node builders expose metadata and dispatch sync value endpoints",
          "[UT][wh/compose/node/tools_builder.hpp][make_tools_node][branch][boundary]") {
  wh::compose::tool_registry registry{};
  registry.emplace(
      "echo", wh::compose::tool_entry{
                  .invoke = [](const wh::compose::tool_call &call,
                               wh::tool::call_scope) -> wh::core::result<wh::compose::graph_value> {
                    return wh::compose::graph_value{call.arguments};
                  },
              });
  wh::compose::tools_options runtime_options{};
  runtime_options.missing = wh::compose::tool_entry{
      .invoke = [](const wh::compose::tool_call &,
                   wh::tool::call_scope) -> wh::core::result<wh::compose::graph_value> {
        return wh::compose::graph_value{std::string{"fallback"}};
      }};

  auto node = wh::compose::make_tools_node("tools", registry, wh::compose::graph_add_node_options{},
                                           runtime_options);
  REQUIRE(node.key() == "tools");
  REQUIRE(node.input_contract() == wh::compose::node_contract::value);
  REQUIRE(node.output_contract() == wh::compose::node_contract::value);
  REQUIRE(node.input_gate().kind == wh::compose::input_gate_kind::value_exact);
  REQUIRE(node.output_gate().kind == wh::compose::output_gate_kind::value_exact);
  REQUIRE(node.exec_mode() == wh::compose::node_exec_mode::sync);
  REQUIRE(node.exec_origin() == wh::compose::node_exec_origin::authored);
  REQUIRE(node.options().type == "tools_node");
  REQUIRE(node.options().label == "tools_node");

  wh::compose::graph graph{};
  REQUIRE(graph.add_tools(node).has_value());
  REQUIRE(graph.add_entry_edge("tools").has_value());
  REQUIRE(graph.add_exit_edge("tools").has_value());
  REQUIRE(graph.compile().has_value());

  auto invoked = invoke_single_node_graph(
      graph, wh::compose::graph_value{make_tool_batch({wh::compose::tool_call{
                 .call_id = "call-1",
                 .tool_name = "echo",
                 .arguments = "payload",
             }})});
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value().output_status.has_value());
  auto results = collect_tool_results(invoked.value().output_status.value());
  REQUIRE(results.has_value());
  REQUIRE(results.value().get().size() == 1U);
  REQUIRE(results.value().get().front().call_id == "call-1");
  REQUIRE(results.value().get().front().tool_name == "echo");
  REQUIRE(*wh::core::any_cast<std::string>(&results.value().get().front().value) == "payload");

  auto missing = invoke_single_node_graph(
      graph, wh::compose::graph_value{make_tool_batch({wh::compose::tool_call{
                 .call_id = "call-2",
                 .tool_name = "missing",
             }})});
  REQUIRE(missing.has_value());
  auto fallback = collect_tool_results(missing.value().output_status.value());
  REQUIRE(fallback.has_value());
  REQUIRE(*wh::core::any_cast<std::string>(&fallback.value().get().front().value) == "fallback");

  auto wrong_input = invoke_single_node_graph(graph, wh::compose::graph_value{7});
  REQUIRE(wrong_input.has_value());
  REQUIRE(wrong_input.value().output_status.has_error());
  REQUIRE(wrong_input.value().output_status.error() == wh::core::errc::type_mismatch);
}

TEST_CASE("tools node builders dispatch sync stream endpoints into tool events",
          "[UT][wh/compose/node/tools_builder.hpp][tools_node::compile][branch]") {
  wh::compose::tool_registry registry{};
  registry.emplace(
      "stream",
      wh::compose::tool_entry{
          .stream = [](const wh::compose::tool_call &call,
                       wh::tool::call_scope) -> wh::core::result<wh::compose::graph_stream_reader> {
            return wh::compose::make_values_stream_reader(std::vector<wh::compose::graph_value>{
                wh::compose::graph_value{call.arguments},
            });
          },
      });

  wh::compose::graph graph{};
  REQUIRE(graph
              .add_tools(wh::compose::make_tools_node<wh::compose::node_contract::value,
                                                      wh::compose::node_contract::stream>(
                  "tools", std::move(registry)))
              .has_value());
  REQUIRE(graph.add_entry_edge("tools").has_value());
  REQUIRE(graph.add_exit_edge("tools").has_value());
  REQUIRE(graph.compile().has_value());

  auto compiled = graph.compiled_node_by_key("tools");
  REQUIRE(compiled.has_value());

  wh::compose::graph_value input = make_tool_batch({wh::compose::tool_call{
      .call_id = "call-stream",
      .tool_name = "stream",
      .arguments = "chunk",
  }});
  wh::core::run_context context{};
  auto status = wh::compose::run_compiled_sync_node(compiled.value().get(), input, context,
                                                    wh::compose::node_runtime{});
  REQUIRE(status.has_value());

  auto reader = read_any<wh::compose::graph_stream_reader>(std::move(status).value());
  REQUIRE(reader.has_value());

  auto events = collect_tool_events(std::move(reader).value());
  REQUIRE(events.has_value());
  REQUIRE(events.value().size() == 1U);
  REQUIRE(events.value().front().call_id == "call-stream");
  REQUIRE(events.value().front().tool_name == "stream");
  REQUIRE(*wh::core::any_cast<std::string>(&events.value().front().value) == "chunk");
}

TEST_CASE("async stream tools node returns graph stream reader payload",
          "[UT][wh/compose/node/tools_builder.hpp][tools_node::compile][branch][async]") {
  wh::compose::tool_registry registry{};
  registry.emplace(
      "stream", wh::compose::tool_entry{
                    .async_stream = [](wh::compose::tool_call call,
                                       wh::tool::call_scope) -> wh::compose::tools_stream_sender {
                      return [](wh::compose::tool_call owned_call)
                                 -> exec::task<wh::core::result<wh::compose::graph_stream_reader>> {
                        auto reader = wh::compose::make_values_stream_reader(
                            std::vector<wh::compose::graph_value>{
                                wh::compose::graph_value{std::move(owned_call.arguments)},
                            });
                        REQUIRE(reader.has_value());
                        co_return wh::core::result<wh::compose::graph_stream_reader>{
                            std::move(reader).value()};
                      }(std::move(call));
                    },
                });

  wh::compose::graph graph{};
  REQUIRE(graph
              .add_tools(wh::compose::make_tools_node<wh::compose::node_contract::value,
                                                      wh::compose::node_contract::stream,
                                                      wh::compose::node_exec_mode::async>(
                  "tools", std::move(registry)))
              .has_value());
  REQUIRE(graph.add_entry_edge("tools").has_value());
  REQUIRE(graph.add_exit_edge("tools").has_value());
  REQUIRE(graph.compile().has_value());

  auto compiled = graph.compiled_node_by_key("tools");
  REQUIRE(compiled.has_value());

  wh::testing::helper::static_thread_scheduler_helper scheduler_helper{1U};
  auto scheduler = wh::core::detail::erase_resume_scheduler(scheduler_helper.scheduler());
  wh::compose::node_runtime runtime{};
  runtime.set_control_scheduler(&scheduler);

  wh::compose::graph_value input = make_tool_batch({wh::compose::tool_call{
      .call_id = "call-stream",
      .tool_name = "stream",
      .arguments = "chunk",
  }});
  wh::core::run_context context{};
  auto waited = stdexec::sync_wait(
      wh::compose::run_compiled_async_node(compiled.value().get(), input, context, runtime));
  REQUIRE(waited.has_value());
  auto status = std::get<0>(std::move(*waited));
  REQUIRE(status.has_value());

  auto reader = read_any<wh::compose::graph_stream_reader>(std::move(status).value());
  REQUIRE(reader.has_value());
}

TEST_CASE("tools node builders also compile async value endpoints and preserve labels",
          "[UT][wh/compose/node/tools_builder.hpp][make_tools_node][condition][branch][boundary]") {
  wh::compose::tool_registry registry{};
  registry.emplace(
      "echo", wh::compose::tool_entry{
                  .async_invoke = [](wh::compose::tool_call call,
                                     wh::tool::call_scope) -> wh::compose::tools_invoke_sender {
                    return [](wh::compose::tool_call owned_call)
                               -> exec::task<wh::core::result<wh::compose::graph_value>> {
                      co_return wh::core::result<wh::compose::graph_value>{
                          wh::compose::graph_value{std::string{"async:"} + owned_call.arguments}};
                    }(std::move(call));
                  },
              });

  auto node = wh::compose::make_tools_node<wh::compose::node_contract::value,
                                           wh::compose::node_contract::value,
                                           wh::compose::node_exec_mode::async>("tools_async_value",
                                                                               registry);
  REQUIRE(node.options().type == "tools_node");
  REQUIRE(node.options().label == "tools_node");

  wh::compose::graph graph{};
  REQUIRE(graph.add_tools(std::move(node)).has_value());
  REQUIRE(graph.add_entry_edge("tools_async_value").has_value());
  REQUIRE(graph.add_exit_edge("tools_async_value").has_value());
  REQUIRE(graph.compile().has_value());

  auto compiled = graph.compiled_node_by_key("tools_async_value");
  REQUIRE(compiled.has_value());

  wh::testing::helper::static_thread_scheduler_helper scheduler_helper{1U};
  auto scheduler = wh::core::detail::erase_resume_scheduler(scheduler_helper.scheduler());
  wh::compose::node_runtime runtime{};
  runtime.set_control_scheduler(&scheduler);

  wh::core::run_context context{};
  wh::compose::graph_value input = make_tool_batch({
      wh::compose::tool_call{
          .call_id = "call-async",
          .tool_name = "echo",
          .arguments = "payload",
      },
  });
  auto waited = stdexec::sync_wait(
      wh::compose::run_compiled_async_node(compiled.value().get(), input, context, runtime));
  REQUIRE(waited.has_value());
  auto status = std::get<0>(std::move(*waited));
  REQUIRE(status.has_value());

  auto results = read_any<std::vector<wh::compose::tool_result>>(std::move(status).value());
  REQUIRE(results.has_value());
  REQUIRE(results.value().size() == 1U);
  REQUIRE(*wh::core::any_cast<std::string>(&results.value().front().value) == "async:payload");
}
