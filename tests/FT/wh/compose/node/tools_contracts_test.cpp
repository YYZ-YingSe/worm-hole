#include <catch2/catch_test_macros.hpp>

#include <exec/static_thread_pool.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "helper/component_contract_support.hpp"
#include "helper/compose_graph_test_utils.hpp"
#include "wh/compose/node.hpp"
#include "wh/schema/tool.hpp"

namespace {

using wh::testing::helper::build_single_node_graph;
using wh::testing::helper::collect_tool_events;
using wh::testing::helper::collect_tool_results;
using wh::testing::helper::execute_single_compiled_node;
using wh::testing::helper::invoke_value_sync;
using wh::testing::helper::make_test_node_runtime;
using wh::testing::helper::make_tool_batch;
using wh::testing::helper::read_graph_value;
using wh::testing::helper::sync_tool_invoke_impl;

} // namespace

TEST_CASE("compose tools node validates tool batch input and missing tools",
          "[core][compose][tools][condition]") {
  wh::compose::tool_registry tools{};
  tools.insert_or_assign(
      "known", wh::compose::tool_entry{
                   .invoke =
                       [](const wh::compose::tool_call &call, wh::tool::call_scope)
                           -> wh::core::result<wh::compose::graph_value> {
                     return wh::core::any(std::string{"known:"} + call.arguments);
                   }});

  wh::compose::tools_options options{};
  options.missing = wh::compose::tool_entry{
      .invoke =
          [](const wh::compose::tool_call &call, wh::tool::call_scope)
              -> wh::core::result<wh::compose::graph_value> {
            return wh::core::any(std::string{"unknown:"} + call.tool_name);
          }};
  auto node = wh::compose::make_tools_node(
      "tools", tools, wh::compose::graph_add_node_options{}, std::move(options));

  SECTION("input must be tool_batch") {
    wh::core::run_context context{};
    auto status = execute_single_compiled_node(
        node, wh::core::any(std::string{"invalid"}), context);
    REQUIRE(status.has_error());
  }

  SECTION("tool batch must contain at least one named call") {
    wh::core::run_context context{};
    auto status =
        execute_single_compiled_node(node, wh::core::any(make_tool_batch({})),
                                     context);
    REQUIRE(status.has_error());

    status = execute_single_compiled_node(
        node,
        wh::core::any(make_tool_batch({wh::compose::tool_call{
            .call_id = "call-1", .tool_name = "", .arguments = "{}"}})),
        context);
    REQUIRE(status.has_error());
  }

  SECTION("missing tool uses configured missing entry") {
    wh::core::run_context context{};
    auto status = execute_single_compiled_node(
        node,
        wh::core::any(make_tool_batch({wh::compose::tool_call{
            .call_id = "call-missing",
            .tool_name = "missing",
            .arguments = "{}"}})),
        context);
    REQUIRE(status.has_value());

    auto results = collect_tool_results(status.value());
    REQUIRE(results.has_value());
    REQUIRE(results.value().get().size() == 1U);
    REQUIRE(results.value().get()[0].call_id == "call-missing");
    REQUIRE(results.value().get()[0].tool_name == "missing");
    auto text = read_graph_value<std::string>(results.value().get()[0].value);
    REQUIRE(text.has_value());
    REQUIRE(text.value() == "unknown:missing");
  }

  SECTION("missing tool fails fast without missing entry") {
    auto strict_node = wh::compose::make_tools_node("tools-strict", tools);
    wh::core::run_context context{};
    auto status = execute_single_compiled_node(
        strict_node,
        wh::core::any(make_tool_batch({wh::compose::tool_call{
            .call_id = "call-missing",
            .tool_name = "missing",
            .arguments = "{}"}})),
        context);
    REQUIRE(status.has_error());
    REQUIRE(status.error() == wh::core::errc::not_found);
  }
}

TEST_CASE("compose tools node observation provides callback-capable tool scope",
          "[core][compose][tools][condition]") {
  wh::schema::tool_schema_definition tool_schema{};
  tool_schema.name = "echo";
  tool_schema.description = "echo";

  wh::compose::tool_registry tools{};
  tools.insert_or_assign(
      "echo", wh::compose::tool_entry{
                  .invoke =
                      [tool_schema](const wh::compose::tool_call &call,
                                    wh::tool::call_scope scope)
                          -> wh::core::result<wh::compose::graph_value> {
                    auto tool_component = wh::tool::tool{
                        tool_schema, sync_tool_invoke_impl{
                                         [](const std::string_view input,
                                            const wh::tool::tool_options &)
                                             -> wh::core::result<std::string> {
                                           return std::string{"tool:"} +
                                                  std::string{input};
                                         }}};
                    auto invoked = tool_component.invoke(
                        wh::tool::tool_request{call.arguments, {}}, scope.run);
                    if (invoked.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(
                          invoked.error());
                    }
                    return wh::core::any(std::move(invoked).value());
                  }});

  wh::compose::graph graph{};
  REQUIRE(graph.add_tools("tools", std::move(tools)).has_value());
  REQUIRE(graph.add_entry_edge("tools").has_value());
  REQUIRE(graph.add_exit_edge("tools").has_value());
  REQUIRE(graph.compile().has_value());

  std::vector<wh::core::callback_run_info> callback_infos{};
  wh::compose::graph_node_callback_registration local_registration{};
  local_registration.config = wh::core::callback_config{
      .timing_checker = wh::core::callback_timing_checker{
          [](const wh::core::callback_stage stage) noexcept {
            return stage == wh::core::callback_stage::end;
          }},
      .name = "tools-local",
  };
  local_registration.callbacks.on_end = wh::core::stage_view_callback{
      [&callback_infos](const wh::core::callback_stage stage,
                        const wh::core::callback_event_view event,
                        const wh::core::callback_run_info &run_info) {
        REQUIRE(stage == wh::core::callback_stage::end);
        REQUIRE(event.get_if<wh::tool::tool_callback_event>() != nullptr);
        callback_infos.push_back(run_info);
      }};

  wh::compose::graph_call_options call_options{};
  call_options.trace = wh::compose::graph_trace_context{
      .trace_id = "trace-tools",
      .parent_span_id = "trace-root",
  };
  call_options.node_observations.push_back(
      wh::compose::graph_node_observation_override{
          .path = wh::compose::make_node_path({"tools"}),
          .local_callbacks = wh::compose::graph_node_callback_plan{
              std::move(local_registration),
          },
      });

  wh::core::run_context context{};
  auto invoked = invoke_value_sync(
      graph,
      wh::core::any(make_tool_batch({wh::compose::tool_call{
          .call_id = "call-1", .tool_name = "echo", .arguments = "x"}})),
      context, std::move(call_options));
  REQUIRE(invoked.has_value());
  REQUIRE(callback_infos.size() == 1U);
  REQUIRE(callback_infos.front().trace_id == "trace-tools");
  REQUIRE_FALSE(callback_infos.front().span_id.empty());
  REQUIRE_FALSE(callback_infos.front().parent_span_id.empty());
  REQUIRE(callback_infos.front().node_path.to_string() == "graph/tools");
}

TEST_CASE("compose tools node supports call-time overrides and middleware",
          "[core][compose][tools][branch]") {
  wh::compose::tool_registry default_tools{};
  default_tools.insert_or_assign(
      "echo", wh::compose::tool_entry{
                  .invoke =
                      [](const wh::compose::tool_call &call, wh::tool::call_scope)
                          -> wh::core::result<wh::compose::graph_value> {
                    return wh::core::any(std::string{"default:"} + call.arguments);
                  }});

  wh::compose::tool_registry override_tools{};
  override_tools.insert_or_assign(
      "echo", wh::compose::tool_entry{
                  .invoke =
                      [](const wh::compose::tool_call &call, wh::tool::call_scope)
                          -> wh::core::result<wh::compose::graph_value> {
                    return wh::core::any(std::string{"override:"} + call.arguments);
                  }});

  wh::compose::tools_options tools_options{};
  tools_options.middleware.push_back(wh::compose::tool_middleware{
      .before =
          [](wh::compose::tool_call &call, const wh::tool::call_scope &)
              -> wh::core::result<void> {
            call.arguments += ":before";
            return {};
          },
      .after =
          [](const wh::compose::tool_call &, wh::compose::graph_value &output,
             const wh::tool::call_scope &) -> wh::core::result<void> {
            auto text = read_graph_value<std::string>(std::move(output));
            if (text.has_error()) {
              return wh::core::result<void>::failure(text.error());
            }
            output = wh::core::any(text.value() + ":after");
            return {};
          },
  });
  auto node = wh::compose::make_tools_node(
      "tools", std::move(default_tools), wh::compose::graph_add_node_options{},
      std::move(tools_options));

  wh::core::run_context context{};
  wh::compose::graph_call_options call_options{};
  call_options.tools = wh::compose::tools_call_options{
      .registry = std::cref(override_tools),
  };
  auto call_scope = wh::compose::graph_call_scope{call_options};

  auto status = execute_single_compiled_node(
      node,
      wh::core::any(make_tool_batch({wh::compose::tool_call{
          .call_id = "call-1", .tool_name = "echo", .arguments = "x"}})),
      context, make_test_node_runtime(std::addressof(call_scope)));
  REQUIRE(status.has_value());
  auto results = collect_tool_results(status.value());
  REQUIRE(results.has_value());
  REQUIRE(results.value().get().size() == 1U);
  auto text = read_graph_value<std::string>(results.value().get()[0].value);
  REQUIRE(text.has_value());
  REQUIRE(text.value() == "override:x:before:after");
}

TEST_CASE("compose tools node isolates per-call run_context mutations",
          "[core][compose][tools][boundary]") {
  wh::compose::tool_registry tools{};
  tools.insert_or_assign(
      "writer", wh::compose::tool_entry{
                    .invoke =
                        [](const wh::compose::tool_call &call,
                           wh::tool::call_scope scope)
                            -> wh::core::result<wh::compose::graph_value> {
                          auto stored = wh::core::set_session_value(
                              scope.run, "tool-scratch", call.arguments);
                          if (stored.has_error()) {
                            return wh::core::result<wh::compose::graph_value>::failure(
                                stored.error());
                          }
                          return wh::core::any(std::string{"writer:"} +
                                               call.arguments);
                        }});
  tools.insert_or_assign(
      "reader", wh::compose::tool_entry{
                    .invoke =
                        [](const wh::compose::tool_call &call,
                           wh::tool::call_scope scope)
                            -> wh::core::result<wh::compose::graph_value> {
                          auto scratch = wh::core::session_value_ref<std::string>(
                              scope.run, "tool-scratch");
                          if (scratch.has_value()) {
                            return wh::core::result<wh::compose::graph_value>::failure(
                                wh::core::errc::contract_violation);
                          }
                          return wh::core::any(std::string{"reader:"} +
                                               call.arguments);
                        }});
  auto node = wh::compose::make_tools_node("tools", std::move(tools));

  wh::core::run_context context{};
  auto status = execute_single_compiled_node(
      node,
      wh::core::any(make_tool_batch(
          {wh::compose::tool_call{.call_id = "call-1",
                                  .tool_name = "writer",
                                  .arguments = "a"},
           wh::compose::tool_call{.call_id = "call-2",
                                  .tool_name = "reader",
                                  .arguments = "b"}})),
      context);
  REQUIRE(status.has_value());
  auto results = collect_tool_results(status.value());
  REQUIRE(results.has_value());
  REQUIRE(results.value().get().size() == 2U);
  auto first = read_graph_value<std::string>(results.value().get()[0].value);
  auto second = read_graph_value<std::string>(results.value().get()[1].value);
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  REQUIRE(first.value() == "writer:a");
  REQUIRE(second.value() == "reader:b");

  auto outer_scratch =
      wh::core::session_value_ref<std::string>(context, "tool-scratch");
  REQUIRE(outer_scratch.has_error());
  REQUIRE(outer_scratch.error() == wh::core::errc::not_found);
}

TEST_CASE("compose sync tools node rejects non-sequential override",
          "[core][compose][tools][condition]") {
  wh::compose::tool_registry tools{};
  tools.insert_or_assign(
      "echo", wh::compose::tool_entry{
                  .invoke =
                      [](const wh::compose::tool_call &call, wh::tool::call_scope)
                          -> wh::core::result<wh::compose::graph_value> {
                    return wh::core::any(call.arguments);
                  }});
  auto node = wh::compose::make_tools_node("tools", std::move(tools));

  wh::compose::graph_call_options call_options{};
  call_options.tools = wh::compose::tools_call_options{
      .sequential = false,
  };
  auto call_scope = wh::compose::graph_call_scope{call_options};
  wh::core::run_context context{};
  auto status = execute_single_compiled_node(
      node,
      wh::core::any(make_tool_batch({wh::compose::tool_call{
          .call_id = "call-1", .tool_name = "echo", .arguments = "x"}})),
      context, make_test_node_runtime(std::addressof(call_scope)));
  REQUIRE(status.has_error());
  REQUIRE(status.error() == wh::core::errc::not_supported);
}

TEST_CASE("compose tools node rejects authored execution-mode conflicts at graph build",
          "[core][compose][tools][boundary]") {
  SECTION("sync tools node rejects authored non-sequential runtime option") {
    wh::compose::tool_registry tools{};
    tools.insert_or_assign(
        "echo", wh::compose::tool_entry{
                    .invoke =
                        [](const wh::compose::tool_call &call, wh::tool::call_scope)
                            -> wh::core::result<wh::compose::graph_value> {
                          return wh::core::any(call.arguments);
                        }});

    wh::compose::tools_options runtime_options{};
    runtime_options.sequential = false;

    wh::compose::graph graph{};
    auto added = graph.add_tools(wh::compose::make_tools_node(
        "tools", std::move(tools), wh::compose::graph_add_node_options{},
        std::move(runtime_options)));
    REQUIRE(added.has_error());
    REQUIRE(added.error() == wh::core::errc::not_supported);
    REQUIRE(graph.diagnostics().back().message.find("non-sequential") !=
            std::string::npos);
  }

  SECTION("async tools node rejects sync-only registry at graph build") {
    wh::compose::tool_registry tools{};
    tools.insert_or_assign(
        "echo", wh::compose::tool_entry{
                    .invoke =
                        [](const wh::compose::tool_call &call, wh::tool::call_scope)
                            -> wh::core::result<wh::compose::graph_value> {
                          return wh::core::any(call.arguments);
                        }});

    wh::compose::graph graph{};
    auto added = graph.add_tools(wh::compose::make_tools_node<
        wh::compose::node_contract::value, wh::compose::node_contract::value,
        wh::compose::node_exec_mode::async>("tools", std::move(tools)));
    REQUIRE(added.has_error());
    REQUIRE(added.error() == wh::core::errc::not_supported);
    REQUIRE(graph.diagnostics().back().message.find("async_invoke") !=
            std::string::npos);
  }
}

TEST_CASE("compose async tools node executes non-sequential mode and preserves order",
          "[core][compose][tools][async][condition]") {
  std::atomic<int> active{0};
  std::atomic<int> max_active{0};
  exec::static_thread_pool worker_pool{3U};

  wh::compose::tool_registry tools{};
  tools.insert_or_assign(
      "slow", wh::compose::tool_entry{
                  .async_invoke =
                      [&worker_pool, &active, &max_active](
                          wh::compose::tool_call call, wh::tool::call_scope)
                          -> wh::compose::tools_invoke_sender {
                    return stdexec::starts_on(
                        worker_pool.get_scheduler(),
                        stdexec::just(std::move(call.arguments)) |
                            stdexec::then(
                                [&active, &max_active](std::string value)
                                    -> wh::core::result<wh::compose::graph_value> {
                                  const auto running = active.fetch_add(1) + 1;
                                  auto previous = max_active.load();
                                  while (previous < running &&
                                         !max_active.compare_exchange_weak(
                                             previous, running)) {
                                  }
                                  std::this_thread::sleep_for(
                                      std::chrono::milliseconds{20});
                                  active.fetch_sub(1);
                                  return wh::core::any(std::move(value));
                                }));
                  }});
  auto node = wh::compose::make_tools_node<
      wh::compose::node_contract::value, wh::compose::node_contract::value,
      wh::compose::node_exec_mode::async>("tools-async-parallel",
                                          std::move(tools));

  wh::compose::graph_call_options call_options{};
  call_options.tools = wh::compose::tools_call_options{
      .sequential = false,
  };
  auto call_scope = wh::compose::graph_call_scope{call_options};

  wh::core::run_context context{};
  auto status = execute_single_compiled_node(
      node,
      wh::core::any(make_tool_batch(
          {wh::compose::tool_call{.call_id = "call-1",
                                  .tool_name = "slow",
                                  .arguments = "a"},
           wh::compose::tool_call{.call_id = "call-2",
                                  .tool_name = "slow",
                                  .arguments = "b"},
           wh::compose::tool_call{.call_id = "call-3",
                                  .tool_name = "slow",
                                  .arguments = "c"}})),
      context, make_test_node_runtime(std::addressof(call_scope)));
  REQUIRE(status.has_value());
  auto results = collect_tool_results(status.value());
  REQUIRE(results.has_value());
  REQUIRE(results.value().get().size() == 3U);
  REQUIRE(results.value().get()[0].call_id == "call-1");
  REQUIRE(results.value().get()[1].call_id == "call-2");
  REQUIRE(results.value().get()[2].call_id == "call-3");
  REQUIRE(read_graph_value<std::string>(results.value().get()[0].value).value() ==
          "a");
  REQUIRE(read_graph_value<std::string>(results.value().get()[1].value).value() ==
          "b");
  REQUIRE(read_graph_value<std::string>(results.value().get()[2].value).value() ==
          "c");
  REQUIRE(max_active.load() >= 2);
}

TEST_CASE("compose async tools node honors runtime parallel gate override",
          "[core][compose][tools][async][condition]") {
  std::atomic<int> active{0};
  std::atomic<int> max_active{0};
  exec::static_thread_pool worker_pool{3U};

  wh::compose::tool_registry tools{};
  tools.insert_or_assign(
      "slow", wh::compose::tool_entry{
                  .async_invoke =
                      [&worker_pool, &active, &max_active](
                          wh::compose::tool_call call, wh::tool::call_scope)
                          -> wh::compose::tools_invoke_sender {
                    return stdexec::starts_on(
                        worker_pool.get_scheduler(),
                        stdexec::just(std::move(call.arguments)) |
                            stdexec::then(
                                [&active, &max_active](std::string value)
                                    -> wh::core::result<wh::compose::graph_value> {
                                  const auto running = active.fetch_add(1) + 1;
                                  auto previous = max_active.load();
                                  while (previous < running &&
                                         !max_active.compare_exchange_weak(
                                             previous, running)) {
                                  }
                                  std::this_thread::sleep_for(
                                      std::chrono::milliseconds{20});
                                  active.fetch_sub(1);
                                  return wh::core::any(std::move(value));
                                }));
                  }});
  auto node = wh::compose::make_tools_node<
      wh::compose::node_contract::value, wh::compose::node_contract::value,
      wh::compose::node_exec_mode::async>("tools-async-gated",
                                          std::move(tools));

  wh::compose::graph_call_options call_options{};
  call_options.tools = wh::compose::tools_call_options{
      .sequential = false,
  };
  auto call_scope = wh::compose::graph_call_scope{call_options};

  wh::core::run_context context{};
  auto status = execute_single_compiled_node(
      node,
      wh::core::any(make_tool_batch(
          {wh::compose::tool_call{.call_id = "call-1",
                                  .tool_name = "slow",
                                  .arguments = "a"},
           wh::compose::tool_call{.call_id = "call-2",
                                  .tool_name = "slow",
                                  .arguments = "b"},
           wh::compose::tool_call{.call_id = "call-3",
                                  .tool_name = "slow",
                                  .arguments = "c"}})),
      context, make_test_node_runtime(std::addressof(call_scope), 1U));
  REQUIRE(status.has_value());
  REQUIRE(max_active.load() == 1);
}

TEST_CASE("compose tools node supports explicit value-to-stream boundary",
          "[core][compose][tools][boundary]") {
  wh::compose::tool_registry tools{};
  tools.insert_or_assign(
      "echo", wh::compose::tool_entry{
                  .stream =
                      [](const wh::compose::tool_call &call, wh::tool::call_scope)
                          -> wh::core::result<wh::compose::graph_stream_reader> {
                    auto [writer, reader] = wh::compose::make_graph_stream();
                    auto first = writer.try_write(wh::core::any(call.arguments + ":1"));
                    if (first.has_error()) {
                      return wh::core::result<wh::compose::graph_stream_reader>::failure(
                          first.error());
                    }
                    auto second = writer.try_write(wh::core::any(call.arguments + ":2"));
                    if (second.has_error()) {
                      return wh::core::result<wh::compose::graph_stream_reader>::failure(
                          second.error());
                    }
                    auto closed = writer.close();
                    if (closed.has_error()) {
                      return wh::core::result<wh::compose::graph_stream_reader>::failure(
                          closed.error());
                    }
                    return std::move(reader);
                  }});

  auto node = wh::compose::make_tools_node<wh::compose::node_contract::value,
                                           wh::compose::node_contract::stream>(
      "tools-stream", std::move(tools));
  auto lowered = build_single_node_graph(node);
  REQUIRE(lowered.has_value());
  REQUIRE(lowered->node->meta.input_contract == wh::compose::node_contract::value);
  REQUIRE(lowered->node->meta.output_contract ==
          wh::compose::node_contract::stream);

  wh::core::run_context context{};
  auto status = execute_single_compiled_node(
      node,
      wh::core::any(make_tool_batch({wh::compose::tool_call{
          .call_id = "call-1", .tool_name = "echo", .arguments = "payload"}})),
      context);
  REQUIRE(status.has_value());

  auto output_stream =
      read_graph_value<wh::compose::graph_stream_reader>(std::move(status).value());
  REQUIRE(output_stream.has_value());
  auto events = collect_tool_events(std::move(output_stream).value());
  REQUIRE(events.has_value());
  REQUIRE(events.value().size() == 2U);
  REQUIRE(events.value()[0].call_id == "call-1");
  REQUIRE(events.value()[0].tool_name == "echo");
  REQUIRE(events.value()[1].call_id == "call-1");
  auto first = read_graph_value<std::string>(events.value()[0].value);
  auto second = read_graph_value<std::string>(events.value()[1].value);
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  REQUIRE(first.value() == "payload:1");
  REQUIRE(second.value() == "payload:2");
}

TEST_CASE("compose tools stream middleware keeps per-call state on merged output",
          "[core][compose][tools][stream]") {
  wh::compose::tool_registry tools{};
  tools.insert_or_assign(
      "echo", wh::compose::tool_entry{
                  .stream =
                      [](const wh::compose::tool_call &call, wh::tool::call_scope)
                          -> wh::core::result<wh::compose::graph_stream_reader> {
                    auto [writer, reader] = wh::compose::make_graph_stream();
                    auto first = writer.try_write(wh::core::any(call.arguments + ":1"));
                    if (first.has_error()) {
                      return wh::core::result<wh::compose::graph_stream_reader>::failure(
                          first.error());
                    }
                    auto second = writer.try_write(wh::core::any(call.arguments + ":2"));
                    if (second.has_error()) {
                      return wh::core::result<wh::compose::graph_stream_reader>::failure(
                          second.error());
                    }
                    auto closed = writer.close();
                    if (closed.has_error()) {
                      return wh::core::result<wh::compose::graph_stream_reader>::failure(
                          closed.error());
                    }
                    return std::move(reader);
                  }});

  wh::compose::tools_options options{};
  options.middleware.push_back(wh::compose::tool_middleware{
      .after =
          [](const wh::compose::tool_call &call, wh::compose::graph_value &output,
             const wh::tool::call_scope &scope) -> wh::core::result<void> {
            auto visit =
                wh::core::session_value_ref<int>(scope.run, "tool-stream-count");
            auto count = visit.has_value() ? visit.value().get() : 0;
            auto stored =
                wh::core::set_session_value(scope.run, "tool-stream-count", count + 1);
            if (stored.has_error()) {
              return wh::core::result<void>::failure(stored.error());
            }

            auto text = read_graph_value<std::string>(std::move(output));
            if (text.has_error()) {
              return wh::core::result<void>::failure(text.error());
            }
            output = wh::core::any(call.call_id + ":" + std::to_string(count) +
                                   ":" + text.value());
            return {};
          },
  });

  auto node = wh::compose::make_tools_node<wh::compose::node_contract::value,
                                           wh::compose::node_contract::stream>(
      "tools-stream", std::move(tools), wh::compose::graph_add_node_options{},
      std::move(options));

  wh::core::run_context context{};
  auto status = execute_single_compiled_node(
      node,
      wh::core::any(make_tool_batch(
          {wh::compose::tool_call{.call_id = "call-1",
                                  .tool_name = "echo",
                                  .arguments = "a"},
           wh::compose::tool_call{.call_id = "call-2",
                                  .tool_name = "echo",
                                  .arguments = "b"}})),
      context);
  REQUIRE(status.has_value());

  auto output_stream =
      read_graph_value<wh::compose::graph_stream_reader>(std::move(status).value());
  REQUIRE(output_stream.has_value());
  auto events = collect_tool_events(std::move(output_stream).value());
  REQUIRE(events.has_value());
  REQUIRE(events.value().size() == 4U);

  auto first = read_graph_value<std::string>(events.value()[0].value);
  auto second = read_graph_value<std::string>(events.value()[1].value);
  auto third = read_graph_value<std::string>(events.value()[2].value);
  auto fourth = read_graph_value<std::string>(events.value()[3].value);
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  REQUIRE(third.has_value());
  REQUIRE(fourth.has_value());
  REQUIRE(first.value() == "call-1:0:a:1");
  REQUIRE(second.value() == "call-2:0:b:1");
  REQUIRE(third.value() == "call-1:1:a:2");
  REQUIRE(fourth.value() == "call-2:1:b:2");
}

TEST_CASE("compose async tools node should preserve graph stream reader payload",
          "[core][compose][tools][async][stream]") {
  wh::compose::tool_registry tools{};
  tools.insert_or_assign(
      "echo", wh::compose::tool_entry{
                  .async_stream =
                      [](wh::compose::tool_call call, wh::tool::call_scope)
                          -> wh::compose::tools_stream_sender {
                    auto reader = wh::compose::make_values_stream_reader(
                        std::vector<wh::compose::graph_value>{
                            wh::core::any(call.arguments + ":1"),
                            wh::core::any(call.arguments + ":2"),
                        });
                    if (reader.has_error()) {
                      return stdexec::just(
                          wh::core::result<wh::compose::graph_stream_reader>::failure(
                              reader.error()));
                    }
                    return stdexec::just(
                        wh::core::result<wh::compose::graph_stream_reader>{
                            std::move(reader).value()});
                  }});

  auto node = wh::compose::make_tools_node<
      wh::compose::node_contract::value, wh::compose::node_contract::stream,
      wh::compose::node_exec_mode::async>("tools-async-stream",
                                          std::move(tools));

  auto compiled = build_single_node_graph(node);
  REQUIRE(compiled.has_value());

  exec::static_thread_pool pool{1U};
  auto graph_scheduler =
      wh::core::detail::erase_resume_scheduler(pool.get_scheduler());
  wh::compose::node_runtime runtime{};
  runtime.set_control_scheduler(&graph_scheduler);

  wh::compose::graph_value input = wh::core::any(make_tool_batch(
      {wh::compose::tool_call{
          .call_id = "call-1", .tool_name = "echo", .arguments = "payload"}}));
  wh::core::run_context context{};
  auto waited = stdexec::sync_wait(wh::compose::run_compiled_async_node(
      *compiled->node, input, context, runtime));
  REQUIRE(waited.has_value());
  auto status = std::get<0>(std::move(waited).value());
  REQUIRE(status.has_value());
  INFO(status.value().info().name);
  auto reader =
      read_graph_value<wh::compose::graph_stream_reader>(std::move(status).value());
  REQUIRE(reader.has_value());
}

TEST_CASE("compose tools node reuses executed tools during rerun",
          "[core][compose][tools][condition]") {
  std::size_t execute_count = 0U;
  wh::compose::tool_registry tools{};
  tools.insert_or_assign(
      "run", wh::compose::tool_entry{
                 .invoke =
                     [&execute_count](const wh::compose::tool_call &call,
                                      wh::tool::call_scope)
                         -> wh::core::result<wh::compose::graph_value> {
                       ++execute_count;
                       return wh::core::any(std::string{"fresh:"} + call.arguments);
                     }});
  auto node = wh::compose::make_tools_node("tools", std::move(tools));

  wh::compose::tools_rerun rerun_state{};
  rerun_state.outputs.insert_or_assign(
      "call-1", wh::core::any(std::string{"cached:first"}));
  rerun_state.ids.insert("call-2");
  wh::compose::graph_call_options call_options{};
  call_options.tools = wh::compose::tools_call_options{
      .rerun = std::addressof(rerun_state),
  };
  auto call_scope = wh::compose::graph_call_scope{call_options};

  wh::core::run_context context{};
  auto status = execute_single_compiled_node(
      node,
      wh::core::any(make_tool_batch(
          {wh::compose::tool_call{.call_id = "call-1",
                                  .tool_name = "run",
                                  .arguments = "a"},
           wh::compose::tool_call{.call_id = "call-2",
                                  .tool_name = "run",
                                  .arguments = "b"}})),
      context, make_test_node_runtime(std::addressof(call_scope)));
  REQUIRE(status.has_value());
  REQUIRE(execute_count == 1U);

  auto results = collect_tool_results(status.value());
  REQUIRE(results.has_value());
  REQUIRE(results.value().get().size() == 2U);
  REQUIRE(read_graph_value<std::string>(results.value().get()[0].value).value() ==
          "cached:first");
  REQUIRE(read_graph_value<std::string>(results.value().get()[1].value).value() ==
          "fresh:b");

  REQUIRE(rerun_state.outputs.contains("call-2"));
  REQUIRE(rerun_state.extra.contains("call-1"));
  REQUIRE(rerun_state.extra.contains("call-2"));
  REQUIRE(read_graph_value<std::string>(rerun_state.extra.at("call-1")).value() ==
          "a");
  REQUIRE(read_graph_value<std::string>(rerun_state.extra.at("call-2")).value() ==
          "b");
}

TEST_CASE("compose tools node return-direct filters the batch",
          "[core][compose][tools][condition]") {
  std::size_t direct_execute_count = 0U;
  std::size_t normal_execute_count = 0U;

  wh::compose::tool_registry tools{};
  tools.insert_or_assign(
      "direct", wh::compose::tool_entry{
                    .invoke =
                        [&direct_execute_count](const wh::compose::tool_call &call,
                                                wh::tool::call_scope)
                            -> wh::core::result<wh::compose::graph_value> {
                      ++direct_execute_count;
                      return wh::core::any(std::string{"direct:"} + call.arguments);
                    },
                    .return_direct = true});
  tools.insert_or_assign(
      "normal", wh::compose::tool_entry{
                    .invoke =
                        [&normal_execute_count](const wh::compose::tool_call &call,
                                                wh::tool::call_scope)
                            -> wh::core::result<wh::compose::graph_value> {
                      ++normal_execute_count;
                      return wh::core::any(std::string{"normal:"} + call.arguments);
                    }});
  auto node = wh::compose::make_tools_node("tools", std::move(tools));

  wh::core::run_context context{};
  auto status = execute_single_compiled_node(
      node,
      wh::core::any(make_tool_batch(
          {wh::compose::tool_call{.call_id = "call-normal-1",
                                  .tool_name = "normal",
                                  .arguments = "a"},
           wh::compose::tool_call{.call_id = "call-direct-1",
                                  .tool_name = "direct",
                                  .arguments = "b"},
           wh::compose::tool_call{.call_id = "call-normal-2",
                                  .tool_name = "normal",
                                  .arguments = "c"},
           wh::compose::tool_call{.call_id = "call-direct-2",
                                  .tool_name = "direct",
                                  .arguments = "d"}})),
      context);
  REQUIRE(status.has_value());
  REQUIRE(direct_execute_count == 2U);
  REQUIRE(normal_execute_count == 0U);

  auto results = collect_tool_results(status.value());
  REQUIRE(results.has_value());
  REQUIRE(results.value().get().size() == 2U);
  REQUIRE(results.value().get()[0].call_id == "call-direct-1");
  REQUIRE(results.value().get()[1].call_id == "call-direct-2");
  REQUIRE(read_graph_value<std::string>(results.value().get()[0].value).value() ==
          "direct:b");
  REQUIRE(read_graph_value<std::string>(results.value().get()[1].value).value() ==
          "direct:d");
}
