#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "helper/compose_graph_test_utils.hpp"
#include "wh/compose/authored.hpp"
#include "wh/compose/graph/diff.hpp"
#include "wh/compose/graph/detail/build.hpp"
#include "wh/compose/graph/detail/compile.hpp"
#include "wh/compose/graph/detail/graph_class.hpp"
#include "wh/compose/graph/pregel.hpp"
#include "wh/compose/graph/stream.hpp"
#include "wh/compose/node/component.hpp"
#include "wh/compose/node/lambda.hpp"
#include "wh/compose/node/passthrough.hpp"
#include "wh/compose/node/subgraph.hpp"
#include "wh/compose/node/tools.hpp"
#include "wh/core/any.hpp"

namespace {

using wh::testing::helper::collect_int_graph_stream;
using wh::testing::helper::invoke_value_sync;
using wh::testing::helper::make_int_add_node;
using wh::testing::helper::make_int_mul_node;
using wh::testing::helper::read_graph_value;

struct int_component {
  auto invoke(int, wh::core::run_context &) const -> wh::core::result<int> { return 1; }
};

struct string_component {
  auto invoke(std::string, wh::core::run_context &) const -> wh::core::result<int> { return 1; }
};

struct tool_result_consumer {
  auto invoke(std::vector<wh::compose::tool_result>, wh::core::run_context &) const
      -> wh::core::result<int> {
    return 1;
  }
};

struct collected_stream_consumer {
  auto invoke(std::vector<wh::compose::graph_value>, wh::core::run_context &) const
      -> wh::core::result<int> {
    return 1;
  }
};

} // namespace

TEST_CASE("compose compiled graph copy keeps compiled runtime index and lazy snapshot valid",
          "[core][compose][graph][condition]") {
  auto make_compiled_copy = []() -> wh::compose::graph {
    wh::compose::graph original{};
    REQUIRE(original.add_lambda(make_int_add_node("inc", 1)).has_value());
    REQUIRE(original.add_entry_edge("inc").has_value());
    REQUIRE(original.add_exit_edge("inc").has_value());
    REQUIRE(original.compile().has_value());
    wh::compose::graph copy = original;
    return copy;
  };

  auto copied = make_compiled_copy();

  wh::core::run_context context{};
  auto invoked = invoke_value_sync(copied, wh::core::any(2), context);
  REQUIRE(invoked.has_value());
  auto output = read_graph_value<int>(std::move(invoked).value());
  REQUIRE(output.has_value());
  REQUIRE(output.value() == 3);

  auto diff = wh::compose::diff_graph(copied, copied);
  REQUIRE(diff.has_value());
  REQUIRE(diff.value().entries.empty());
}

TEST_CASE("compose graph compile cold-data retention is configurable",
          "[core][compose][graph][condition]") {
  wh::compose::graph retained{};
  REQUIRE(retained.add_lambda(make_int_add_node("inc", 1)).has_value());
  REQUIRE(retained.add_entry_edge("inc").has_value());
  REQUIRE(retained.add_exit_edge("inc").has_value());
  REQUIRE(retained.compile().has_value());
  REQUIRE(!retained.compile_order().empty());

  wh::compose::graph_compile_options compact_options{};
  compact_options.retain_cold_data = false;
  wh::compose::graph compact{compact_options};
  REQUIRE(compact.add_lambda(make_int_add_node("inc", 1)).has_value());
  REQUIRE(compact.add_entry_edge("inc").has_value());
  REQUIRE(compact.add_exit_edge("inc").has_value());
  REQUIRE(compact.compile().has_value());
  REQUIRE(compact.compile_order().empty());

  wh::core::run_context context{};
  auto invoked = invoke_value_sync(compact, wh::core::any(3), context);
  REQUIRE(invoked.has_value());
  auto output = read_graph_value<int>(std::move(invoked).value());
  REQUIRE(output.has_value());
  REQUIRE(output.value() == 4);
}

TEST_CASE("compose graph compile callback receives compile snapshot",
          "[core][compose][graph][condition]") {
  std::optional<wh::compose::graph_compile_info> compile_snapshot{};

  wh::compose::graph_compile_options options{};
  options.node_timeout = std::chrono::milliseconds{33};
  options.max_parallel_nodes = 4U;
  options.max_parallel_per_node = 2U;
  options.compile_callback = wh::compose::graph_compile_callback{
      [&compile_snapshot](const wh::compose::graph_compile_info &info) -> wh::core::result<void> {
        compile_snapshot = info;
        return {};
      }};

  wh::compose::graph graph{std::move(options)};
  auto worker = make_int_add_node("worker", 1);
  worker.mutable_options().state.require_pre();
  worker.mutable_options().observation.callbacks_enabled = false;
  worker.mutable_options().observation.allow_invoke_override = false;
  worker.mutable_options().observation.local_callbacks.push_back(
      wh::compose::graph_node_callback_registration{});
  worker.mutable_options().retry_budget_override = 4U;
  worker.mutable_options().timeout_override = std::chrono::milliseconds{7};
  worker.mutable_options().max_parallel_override = 1U;
  worker.mutable_options().subgraph_compile_info = wh::compose::graph_compile_info{
      .name = "child-workflow",
      .mode = wh::compose::graph_runtime_mode::dag,
  };

  REQUIRE(graph.add_lambda(std::move(worker)).has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  REQUIRE(compile_snapshot.has_value());
  REQUIRE(compile_snapshot->node_timeout.has_value());
  REQUIRE(compile_snapshot->node_timeout.value() == std::chrono::milliseconds{33});
  REQUIRE(compile_snapshot->max_parallel_nodes == 4U);
  REQUIRE(compile_snapshot->max_parallel_per_node == 2U);
  REQUIRE(compile_snapshot->state_generator_enabled);
  REQUIRE(compile_snapshot->node_key_to_id.contains("worker"));
  REQUIRE(compile_snapshot->subgraphs.contains("worker"));
  REQUIRE(compile_snapshot->subgraphs.at("worker").name == "child-workflow");
  REQUIRE(
      std::any_of(compile_snapshot->control_edges.begin(), compile_snapshot->control_edges.end(),
                  [](const wh::compose::graph_edge &edge) {
                    return edge.from == wh::compose::graph_start_node_key && edge.to == "worker";
                  }));
  REQUIRE(std::any_of(compile_snapshot->data_edges.begin(), compile_snapshot->data_edges.end(),
                      [](const wh::compose::graph_edge &edge) {
                        return edge.from == "worker" && edge.to == wh::compose::graph_end_node_key;
                      }));

  const auto worker_iter = std::find_if(
      compile_snapshot->nodes.begin(), compile_snapshot->nodes.end(),
      [](const wh::compose::graph_compile_node_info &node) { return node.key == "worker"; });
  REQUIRE(worker_iter != compile_snapshot->nodes.end());
  REQUIRE(worker_iter->has_sender);
  REQUIRE(worker_iter->has_subgraph);
  REQUIRE(worker_iter->field_mapping.input_key == worker_iter->options.input_key);
  REQUIRE(worker_iter->field_mapping.output_key == worker_iter->options.output_key);
  REQUIRE_FALSE(worker_iter->options.observation.callbacks_enabled);
  REQUIRE_FALSE(worker_iter->options.observation.allow_invoke_override);
  REQUIRE(worker_iter->options.observation.local_callback_count == 1U);
  REQUIRE(worker_iter->options.state_handlers.pre);
  REQUIRE(worker_iter->options.retry_budget_override.has_value());
  REQUIRE(*worker_iter->options.retry_budget_override == 4U);
  REQUIRE(worker_iter->options.timeout_override.has_value());
  REQUIRE(worker_iter->options.timeout_override.value() == std::chrono::milliseconds{7});
  REQUIRE(worker_iter->options.max_parallel_override.has_value());
  REQUIRE(worker_iter->options.max_parallel_override.value() == 1U);
}

TEST_CASE("compose graph compile callback failure aborts compile",
          "[core][compose][graph][condition]") {
  bool callback_called = false;
  wh::compose::graph_compile_options options{};
  options.compile_callback = wh::compose::graph_compile_callback{
      [&callback_called](const wh::compose::graph_compile_info &) -> wh::core::result<void> {
        callback_called = true;
        return wh::core::result<void>::failure(wh::core::errc::contract_violation);
      }};

  wh::compose::graph graph{std::move(options)};
  REQUIRE(graph.add_lambda(make_int_add_node("worker", 1)).has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());

  auto compiled = graph.compile();
  REQUIRE(compiled.has_error());
  REQUIRE(compiled.error() == wh::core::errc::contract_violation);
  REQUIRE(callback_called);
}

TEST_CASE("compose graph compile requires one explicit path from START to END",
          "[core][compose][graph][condition]") {
  wh::compose::graph graph{};
  REQUIRE(graph.add_lambda(make_int_add_node("worker", 1)).has_value());

  auto compiled = graph.compile();
  REQUIRE(compiled.has_error());
  REQUIRE(compiled.error() == wh::core::errc::not_found);
  REQUIRE_FALSE(graph.diagnostics().empty());
  REQUIRE(graph.diagnostics().back().message == "no reachable path from START to END");
}

TEST_CASE("compose graph keeps first build error stable after duplicate edge",
          "[core][compose][graph][branch]") {
  wh::compose::graph graph{};
  REQUIRE(graph
              .add_lambda("inc",
                          [](const wh::compose::graph_value &input, wh::core::run_context &,
                             const wh::compose::graph_call_scope &)
                              -> wh::core::result<wh::compose::graph_value> { return input; })
              .has_value());
  REQUIRE(graph.add_entry_edge("inc").has_value());
  REQUIRE(graph.add_exit_edge("inc").has_value());

  auto duplicate = graph.add_exit_edge("inc");
  REQUIRE(duplicate.has_error());
  REQUIRE(duplicate.error() == wh::core::errc::already_exists);

  auto compiled = graph.compile();
  REQUIRE(compiled.has_error());
  REQUIRE(compiled.error() == wh::core::errc::already_exists);
}

TEST_CASE("compose graph rejects duplicate output keys during compile",
          "[core][compose][graph][boundary]") {
  wh::compose::graph graph{};

  auto left = wh::compose::make_lambda_node(
      "left",
      [](const wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
        return input;
      });
  left.mutable_options().output_key = "shared";
  auto right = wh::compose::make_lambda_node(
      "right",
      [](const wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
        return input;
      });
  right.mutable_options().output_key = "shared";

  REQUIRE(graph.add_lambda(std::move(left)).has_value());
  REQUIRE(graph.add_lambda(std::move(right)).has_value());
  REQUIRE(graph.add_entry_edge("left").has_value());
  REQUIRE(graph.add_entry_edge("right").has_value());
  REQUIRE(graph.add_exit_edge("left").has_value());
  REQUIRE(graph.add_exit_edge("right").has_value());

  auto compiled = graph.compile();
  REQUIRE(compiled.has_error());
  REQUIRE(compiled.error() == wh::core::errc::already_exists);
}

TEST_CASE("compose graph rejects invalid noControl and noData combination",
          "[core][compose][graph][branch]") {
  wh::compose::graph graph{};
  REQUIRE(graph
              .add_lambda("a",
                          [](const wh::compose::graph_value &input, wh::core::run_context &,
                             const wh::compose::graph_call_scope &)
                              -> wh::core::result<wh::compose::graph_value> { return input; })
              .has_value());
  REQUIRE(graph
              .add_lambda("b",
                          [](const wh::compose::graph_value &input, wh::core::run_context &,
                             const wh::compose::graph_call_scope &)
                              -> wh::core::result<wh::compose::graph_value> { return input; })
              .has_value());
  REQUIRE(graph.add_entry_edge("a").has_value());

  auto invalid = graph.add_edge("a", "b",
                                wh::compose::edge_options{
                                    .no_control = true,
                                    .no_data = true,
                                });
  REQUIRE(invalid.has_error());
  REQUIRE(invalid.error() == wh::core::errc::invalid_argument);
}

TEST_CASE("compose graph validates node timeout retry-window composability",
          "[core][compose][graph][boundary]") {
  SECTION("timeout must be smaller than retry window") {
    wh::compose::graph graph{};
    auto worker = wh::compose::make_passthrough_node("worker");
    worker.mutable_options().timeout_override = std::chrono::milliseconds{10};
    worker.mutable_options().retry_window_override = std::chrono::milliseconds{10};
    REQUIRE(graph.add_passthrough(std::move(worker)).has_value());
    REQUIRE(graph.add_entry_edge("worker").has_value());
    REQUIRE(graph.add_exit_edge("worker").has_value());
    auto compiled = graph.compile();
    REQUIRE(compiled.has_error());
    REQUIRE(compiled.error() == wh::core::errc::invalid_argument);
  }

  SECTION("strictly larger retry window passes") {
    wh::compose::graph graph{};
    auto worker = wh::compose::make_passthrough_node("worker");
    worker.mutable_options().timeout_override = std::chrono::milliseconds{8};
    worker.mutable_options().retry_window_override = std::chrono::milliseconds{20};
    REQUIRE(graph.add_passthrough(std::move(worker)).has_value());
    REQUIRE(graph.add_entry_edge("worker").has_value());
    REQUIRE(graph.add_exit_edge("worker").has_value());
    auto compiled = graph.compile();
    REQUIRE(compiled.has_value());
  }
}

TEST_CASE("compose graph compile surfaces authored subgraph compile failures",
          "[core][compose][graph][boundary]") {
  wh::compose::graph_compile_options child_options{};
  child_options.max_steps = 0U;
  wh::compose::graph child{std::move(child_options)};
  REQUIRE(child.add_passthrough("worker").has_value());
  REQUIRE(child.add_entry_edge("worker").has_value());
  REQUIRE(child.add_exit_edge("worker").has_value());

  wh::compose::graph parent{};
  REQUIRE(
      parent.add_subgraph(wh::compose::make_subgraph_node("child", std::move(child))).has_value());
  REQUIRE(parent.add_entry_edge("child").has_value());
  REQUIRE(parent.add_exit_edge("child").has_value());

  auto compiled = parent.compile();
  REQUIRE(compiled.has_error());
  REQUIRE(compiled.error() == wh::core::errc::invalid_argument);
  REQUIRE(parent.diagnostics().back().message.find("node compile failed: child") !=
          std::string::npos);
}

TEST_CASE("compose graph compile rejects impossible typed edges",
          "[core][compose][graph][boundary]") {
  SECTION("passthrough propagation keeps upstream exact value type") {
    wh::compose::graph graph{};
    REQUIRE(graph
                .add_component(
                    wh::compose::make_component_node<wh::core::component_kind::custom,
                                                     wh::compose::node_contract::value,
                                                     wh::compose::node_contract::value, int, int>(
                        "producer", int_component{}))
                .has_value());
    REQUIRE(graph.add_passthrough(wh::compose::make_passthrough_node("relay")).has_value());
    REQUIRE(graph
                .add_component(wh::compose::make_component_node<
                               wh::core::component_kind::custom, wh::compose::node_contract::value,
                               wh::compose::node_contract::value, std::string, int>(
                    "consumer", string_component{}))
                .has_value());
    REQUIRE(graph.add_entry_edge("producer").has_value());
    REQUIRE(graph.add_edge("producer", "relay").has_value());
    REQUIRE(graph.add_edge("relay", "consumer").has_value());
    REQUIRE(graph.add_exit_edge("consumer").has_value());

    auto compiled = graph.compile();
    REQUIRE(compiled.has_error());
    REQUIRE(compiled.error() == wh::core::errc::contract_violation);
    REQUIRE(graph.diagnostics().back().message.find("relay -> consumer") != std::string::npos);
  }

  SECTION("tools exact value output rejects mismatched typed consumer") {
    wh::compose::graph graph{};
    REQUIRE(graph.add_tools(wh::compose::make_tools_node("tools", wh::compose::tool_registry{}))
                .has_value());
    REQUIRE(graph
                .add_component(
                    wh::compose::make_component_node<wh::core::component_kind::custom,
                                                     wh::compose::node_contract::value,
                                                     wh::compose::node_contract::value, int, int>(
                        "consumer", int_component{}))
                .has_value());
    REQUIRE(graph.add_entry_edge("tools").has_value());
    REQUIRE(graph.add_edge("tools", "consumer").has_value());
    REQUIRE(graph.add_exit_edge("consumer").has_value());

    auto compiled = graph.compile();
    REQUIRE(compiled.has_error());
    REQUIRE(compiled.error() == wh::core::errc::contract_violation);
    REQUIRE(graph.diagnostics().back().message.find("tools -> consumer") != std::string::npos);
  }

  SECTION("tools exact value output accepts matching typed consumer") {
    wh::compose::graph graph{};
    REQUIRE(graph.add_tools(wh::compose::make_tools_node("tools", wh::compose::tool_registry{}))
                .has_value());
    REQUIRE(
        graph
            .add_component(
                wh::compose::make_component_node<
                    wh::core::component_kind::custom, wh::compose::node_contract::value,
                    wh::compose::node_contract::value, std::vector<wh::compose::tool_result>, int>(
                    "consumer", tool_result_consumer{}))
            .has_value());
    REQUIRE(graph.add_entry_edge("tools").has_value());
    REQUIRE(graph.add_edge("tools", "consumer").has_value());
    REQUIRE(graph.add_exit_edge("consumer").has_value());
    REQUIRE(graph.compile().has_value());
  }

  SECTION("map lambda exact input rejects scalar upstream value") {
    wh::compose::graph graph{};
    REQUIRE(graph
                .add_component(
                    wh::compose::make_component_node<wh::core::component_kind::custom,
                                                     wh::compose::node_contract::value,
                                                     wh::compose::node_contract::value, int, int>(
                        "producer", int_component{}))
                .has_value());
    REQUIRE(graph
                .add_lambda(wh::compose::make_lambda_node(
                    "transform",
                    [](wh::compose::graph_value_map &input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_value_map> { return input; }))
                .has_value());
    REQUIRE(graph.add_entry_edge("producer").has_value());
    REQUIRE(graph.add_edge("producer", "transform").has_value());
    REQUIRE(graph.add_exit_edge("transform").has_value());

    auto compiled = graph.compile();
    REQUIRE(compiled.has_error());
    REQUIRE(compiled.error() == wh::core::errc::contract_violation);
    REQUIRE(graph.diagnostics().back().message.find("producer -> transform") != std::string::npos);
  }

  SECTION("value_to_stream edge lowers automatically from node contracts") {
    wh::compose::graph graph{};
    REQUIRE(graph
                .add_component(
                    wh::compose::make_component_node<wh::core::component_kind::custom,
                                                     wh::compose::node_contract::value,
                                                     wh::compose::node_contract::value, int, int>(
                        "producer", int_component{}))
                .has_value());
    REQUIRE(graph
                .add_lambda<wh::compose::node_contract::stream, wh::compose::node_contract::value>(
                    "consumer",
                    [](wh::compose::graph_stream_reader input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_value> {
                      auto chunks = wh::compose::collect_graph_stream_reader(std::move(input));
                      if (chunks.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(chunks.error());
                      }
                      return wh::core::any(static_cast<int>(chunks.value().size()));
                    })
                .has_value());
    REQUIRE(graph.add_entry_edge("producer").has_value());
    REQUIRE(graph.add_edge("producer", "consumer").has_value());
    REQUIRE(graph.add_exit_edge("consumer").has_value());
    REQUIRE(graph.compile().has_value());
  }

  SECTION("stream_to_value edge lowers automatically from node contracts") {
    wh::compose::graph graph{};
    REQUIRE(graph
                .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream>(
                    "source",
                    [](wh::compose::graph_value &, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_stream_reader> {
                      return wh::compose::make_single_value_stream_reader(wh::core::any(1));
                    })
                .has_value());
    REQUIRE(
        graph
            .add_component(
                wh::compose::make_component_node<
                    wh::core::component_kind::custom, wh::compose::node_contract::value,
                    wh::compose::node_contract::value, std::vector<wh::compose::graph_value>, int>(
                    "consumer", collected_stream_consumer{}))
            .has_value());
    REQUIRE(graph.add_entry_edge("source").has_value());
    REQUIRE(graph.add_edge("source", "consumer").has_value());
    REQUIRE(graph.add_exit_edge("consumer").has_value());
    REQUIRE(graph.compile().has_value());
  }

  SECTION("default stream_to_value lowering exposes collected vector type") {
    wh::compose::graph graph{};
    REQUIRE(graph
                .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream>(
                    "source",
                    [](wh::compose::graph_value &, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_stream_reader> {
                      return wh::compose::make_single_value_stream_reader(wh::core::any(1));
                    })
                .has_value());
    REQUIRE(
        graph
            .add_component(
                wh::compose::make_component_node<
                    wh::core::component_kind::custom, wh::compose::node_contract::value,
                    wh::compose::node_contract::value, std::vector<wh::compose::graph_value>, int>(
                    "consumer", collected_stream_consumer{}))
            .has_value());
    REQUIRE(graph.add_entry_edge("source").has_value());
    REQUIRE(graph.add_edge("source", "consumer").has_value());
    REQUIRE(graph.add_exit_edge("consumer").has_value());
    REQUIRE(graph.compile().has_value());
  }

  SECTION("default stream_to_value lowering rejects impossible typed consumer") {
    wh::compose::graph graph{};
    REQUIRE(graph
                .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream>(
                    "source",
                    [](wh::compose::graph_value &, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_stream_reader> {
                      return wh::compose::make_single_value_stream_reader(wh::core::any(1));
                    })
                .has_value());
    REQUIRE(graph
                .add_component(
                    wh::compose::make_component_node<wh::core::component_kind::custom,
                                                     wh::compose::node_contract::value,
                                                     wh::compose::node_contract::value, int, int>(
                        "consumer", int_component{}))
                .has_value());
    REQUIRE(graph.add_entry_edge("source").has_value());
    REQUIRE(graph.add_edge("source", "consumer").has_value());
    REQUIRE(graph.add_exit_edge("consumer").has_value());

    auto compiled = graph.compile();
    REQUIRE(compiled.has_error());
    REQUIRE(compiled.error() == wh::core::errc::contract_violation);
    REQUIRE(graph.diagnostics().back().message.find("source -> consumer") != std::string::npos);
  }

  SECTION("tools stream output default stream_to_value lowering exposes collected vector type") {
    wh::compose::tool_registry tools{};
    tools.insert_or_assign(
        "echo", wh::compose::tool_entry{.stream = [](const wh::compose::tool_call &call,
                                                     wh::tool::call_scope)
                                            -> wh::core::result<wh::compose::graph_stream_reader> {
          auto [writer, reader] = wh::compose::make_graph_stream();
          auto wrote = writer.try_write(wh::core::any(call.arguments));
          if (wrote.has_error()) {
            return wh::core::result<wh::compose::graph_stream_reader>::failure(wrote.error());
          }
          auto closed = writer.close();
          if (closed.has_error()) {
            return wh::core::result<wh::compose::graph_stream_reader>::failure(closed.error());
          }
          return std::move(reader);
        }});

    wh::compose::graph graph{};
    REQUIRE(graph
                .add_tools(wh::compose::make_tools_node<wh::compose::node_contract::value,
                                                        wh::compose::node_contract::stream>(
                    "tools", std::move(tools)))
                .has_value());
    REQUIRE(
        graph
            .add_component(
                wh::compose::make_component_node<
                    wh::core::component_kind::custom, wh::compose::node_contract::value,
                    wh::compose::node_contract::value, std::vector<wh::compose::graph_value>, int>(
                    "consumer", collected_stream_consumer{}))
            .has_value());
    REQUIRE(graph.add_entry_edge("tools").has_value());
    REQUIRE(graph.add_edge("tools", "consumer").has_value());
    REQUIRE(graph.add_exit_edge("consumer").has_value());
    REQUIRE(graph.compile().has_value());
  }

  SECTION(
      "tools stream output default stream_to_value lowering rejects impossible typed consumer") {
    wh::compose::tool_registry tools{};
    tools.insert_or_assign(
        "echo", wh::compose::tool_entry{.stream = [](const wh::compose::tool_call &call,
                                                     wh::tool::call_scope)
                                            -> wh::core::result<wh::compose::graph_stream_reader> {
          auto [writer, reader] = wh::compose::make_graph_stream();
          auto wrote = writer.try_write(wh::core::any(call.arguments));
          if (wrote.has_error()) {
            return wh::core::result<wh::compose::graph_stream_reader>::failure(wrote.error());
          }
          auto closed = writer.close();
          if (closed.has_error()) {
            return wh::core::result<wh::compose::graph_stream_reader>::failure(closed.error());
          }
          return std::move(reader);
        }});

    wh::compose::graph graph{};
    REQUIRE(graph
                .add_tools(wh::compose::make_tools_node<wh::compose::node_contract::value,
                                                        wh::compose::node_contract::stream>(
                    "tools", std::move(tools)))
                .has_value());
    REQUIRE(graph
                .add_component(
                    wh::compose::make_component_node<wh::core::component_kind::custom,
                                                     wh::compose::node_contract::value,
                                                     wh::compose::node_contract::value, int, int>(
                        "consumer", int_component{}))
                .has_value());
    REQUIRE(graph.add_entry_edge("tools").has_value());
    REQUIRE(graph.add_edge("tools", "consumer").has_value());
    REQUIRE(graph.add_exit_edge("consumer").has_value());

    auto compiled = graph.compile();
    REQUIRE(compiled.has_error());
    REQUIRE(compiled.error() == wh::core::errc::contract_violation);
    REQUIRE(graph.diagnostics().back().message.find("tools -> consumer") != std::string::npos);
  }

  SECTION("subgraph stream output default stream_to_value lowering exposes collected vector type") {
    wh::compose::graph child{wh::compose::graph_boundary{
        .output = wh::compose::node_contract::stream,
    }};
    REQUIRE(child
                .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream>(
                    "leaf",
                    [](const wh::compose::graph_value &, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_stream_reader> {
                      return wh::compose::make_single_value_stream_reader(wh::core::any(1));
                    })
                .has_value());
    REQUIRE(child.add_entry_edge("leaf").has_value());
    REQUIRE(child.add_exit_edge("leaf").has_value());
    REQUIRE(child.compile().has_value());

    wh::compose::graph graph{};
    REQUIRE(graph.add_subgraph(wh::compose::make_subgraph_node("source", std::move(child)))
                .has_value());
    REQUIRE(
        graph
            .add_component(
                wh::compose::make_component_node<
                    wh::core::component_kind::custom, wh::compose::node_contract::value,
                    wh::compose::node_contract::value, std::vector<wh::compose::graph_value>, int>(
                    "consumer", collected_stream_consumer{}))
            .has_value());
    REQUIRE(graph.add_entry_edge("source").has_value());
    REQUIRE(graph.add_edge("source", "consumer").has_value());
    REQUIRE(graph.add_exit_edge("consumer").has_value());
    REQUIRE(graph.compile().has_value());
  }

  SECTION(
      "subgraph stream output default stream_to_value lowering rejects impossible typed consumer") {
    wh::compose::graph child{wh::compose::graph_boundary{
        .output = wh::compose::node_contract::stream,
    }};
    REQUIRE(child
                .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream>(
                    "leaf",
                    [](const wh::compose::graph_value &, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_stream_reader> {
                      return wh::compose::make_single_value_stream_reader(wh::core::any(1));
                    })
                .has_value());
    REQUIRE(child.add_entry_edge("leaf").has_value());
    REQUIRE(child.add_exit_edge("leaf").has_value());
    REQUIRE(child.compile().has_value());

    wh::compose::graph graph{};
    REQUIRE(graph.add_subgraph(wh::compose::make_subgraph_node("source", std::move(child)))
                .has_value());
    REQUIRE(graph
                .add_component(
                    wh::compose::make_component_node<wh::core::component_kind::custom,
                                                     wh::compose::node_contract::value,
                                                     wh::compose::node_contract::value, int, int>(
                        "consumer", int_component{}))
                .has_value());
    REQUIRE(graph.add_entry_edge("source").has_value());
    REQUIRE(graph.add_edge("source", "consumer").has_value());
    REQUIRE(graph.add_exit_edge("consumer").has_value());

    auto compiled = graph.compile();
    REQUIRE(compiled.has_error());
    REQUIRE(compiled.error() == wh::core::errc::contract_violation);
    REQUIRE(graph.diagnostics().back().message.find("source -> consumer") != std::string::npos);
  }
}

TEST_CASE("compose graph compile-options snapshot preserves runtime fields",
          "[core][compose][compile-options][condition]") {
  wh::compose::graph_compile_options compile_options{};
  compile_options.name = "task11";
  compile_options.max_steps = 77U;
  compile_options.dispatch_policy = wh::compose::graph_dispatch_policy::next_wave;
  compile_options.retain_cold_data = false;
  compile_options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
  compile_options.fan_in_policy = wh::compose::graph_fan_in_policy::require_all_sources;
  compile_options.retry_budget = 3U;
  compile_options.node_timeout = std::chrono::milliseconds{42};
  compile_options.max_parallel_nodes = 9U;
  compile_options.max_parallel_per_node = 5U;

  wh::compose::pregel pregel{compile_options};
  auto snapshot = pregel.compile_options_snapshot();
  REQUIRE(snapshot.name == "task11");
  REQUIRE(snapshot.mode == wh::compose::graph_runtime_mode::pregel);
  REQUIRE(snapshot.max_steps == 77U);
  REQUIRE(snapshot.dispatch_policy == wh::compose::graph_dispatch_policy::next_wave);
  REQUIRE(!snapshot.retain_cold_data);
  REQUIRE(snapshot.trigger_mode == wh::compose::graph_trigger_mode::all_predecessors);
  REQUIRE(snapshot.fan_in_policy == wh::compose::graph_fan_in_policy::require_all_sources);
  REQUIRE(snapshot.retry_budget == 3U);
  REQUIRE(snapshot.node_timeout.has_value());
  REQUIRE(snapshot.node_timeout.value() == std::chrono::milliseconds{42});
  REQUIRE(snapshot.max_parallel_nodes == 9U);
  REQUIRE(snapshot.max_parallel_per_node == 5U);
}

TEST_CASE("compose graph exact scalar value fan-in fails at graph compile",
          "[core][compose][graph][branch]") {
  struct join_component {
    auto invoke(int value, wh::core::run_context &) const -> wh::core::result<int> {
      return value + 1;
    }
  };

  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::dag;
  options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
  options.fan_in_policy = wh::compose::graph_fan_in_policy::require_all_sources;
  wh::compose::graph graph{std::move(options)};

  REQUIRE(graph.add_lambda(make_int_add_node("a", 10)).has_value());
  REQUIRE(graph.add_lambda(make_int_add_node("b", 20)).has_value());
  REQUIRE(graph
              .add_component(wh::compose::make_component_node<
                             wh::core::component_kind::custom, wh::compose::node_contract::value,
                             wh::compose::node_contract::value, int, int>("join", join_component{}))
              .has_value());
  REQUIRE(graph.add_entry_edge("a").has_value());
  REQUIRE(graph.add_entry_edge("b").has_value());
  REQUIRE(graph.add_edge("a", "join").has_value());
  REQUIRE(graph.add_edge("b", "join").has_value());
  REQUIRE(graph.add_exit_edge("join").has_value());

  auto compiled = graph.compile();
  REQUIRE(compiled.has_error());
  REQUIRE(compiled.error() == wh::core::errc::contract_violation);
  REQUIRE(graph.diagnostics().back().message.find("node=join") != std::string::npos);
}

TEST_CASE("compose graph compiles validates edges and executes invoke path",
          "[core][compose][graph][condition]") {
  wh::compose::graph graph{};
  REQUIRE(graph.add_lambda(make_int_add_node("inc", 1)).has_value());
  REQUIRE(graph.add_lambda(make_int_mul_node("mul", 2)).has_value());
  REQUIRE(graph.add_entry_edge("inc").has_value());
  REQUIRE(graph.add_edge("inc", "mul").has_value());
  REQUIRE(graph.add_exit_edge("mul").has_value());

  REQUIRE(graph.compile().has_value());
  const auto diagnostics = graph.diagnostics();
  const auto compile_diag = std::find_if(diagnostics.begin(), diagnostics.end(),
                                         [](const wh::compose::graph_diagnostic &diagnostic) {
                                           return diagnostic.message.find("compile_options:") == 0U;
                                         });
  REQUIRE(compile_diag != diagnostics.end());

  wh::core::run_context context{};
  auto invoked = invoke_value_sync(graph, wh::core::any(2), context);
  REQUIRE(invoked.has_value());
  auto output = read_graph_value<int>(std::move(invoked).value());
  REQUIRE(output.has_value());
  REQUIRE(output.value() == 6);
}

TEST_CASE("compose graph allow-no-control node forms one explicit root path",
          "[core][compose][graph][boundary]") {
  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::dag;
  wh::compose::graph graph{std::move(options)};

  wh::compose::graph_add_node_options node_options{};
  node_options.allow_no_control = true;
  node_options.allow_no_data = true;
  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::stream, wh::compose::node_contract::value>(
                  "sink",
                  [](wh::compose::graph_stream_reader input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    if (!input.is_source_closed()) {
                      return wh::core::result<wh::compose::graph_value>::failure(
                          wh::core::errc::contract_violation);
                    }
                    auto values = collect_int_graph_stream(std::move(input));
                    if (values.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(values.error());
                    }
                    return wh::core::any(static_cast<int>(values.value().size()));
                  },
                  std::move(node_options))
              .has_value());
  REQUIRE(graph.add_exit_edge("sink").has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context context{};
  auto invoked = invoke_value_sync(graph, wh::core::any(42), context);
  REQUIRE(invoked.has_value());
  auto typed = read_graph_value<int>(std::move(invoked).value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == 0);
}
