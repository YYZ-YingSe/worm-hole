#include <string>
#include <tuple>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "helper/compose_graph_runtime_support.hpp"
#include "helper/compose_graph_test_utils.hpp"
#include "wh/compose/graph.hpp"
#include "wh/compose/graph/detail/dag.hpp"
#include "wh/compose/graph/detail/dag_input.hpp"
#include "wh/compose/node.hpp"

TEST_CASE("dag input builds merged value-map fan-in for require-all predecessors",
          "[UT][wh/compose/graph/detail/"
          "dag_input.hpp][graph::build_node_input_sender][condition][branch][boundary]") {
  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::dag;
  options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
  options.fan_in_policy = wh::compose::graph_fan_in_policy::require_all_sources;
  wh::compose::graph graph{std::move(options)};

  std::size_t merged_count = 0U;
  REQUIRE(graph
              .add_lambda("a",
                          [](const wh::compose::graph_value &, wh::core::run_context &,
                             const wh::compose::graph_call_scope &)
                              -> wh::core::result<wh::compose::graph_value> {
                            return wh::compose::graph_value{10};
                          })
              .has_value());
  REQUIRE(graph
              .add_lambda("b",
                          [](const wh::compose::graph_value &, wh::core::run_context &,
                             const wh::compose::graph_call_scope &)
                              -> wh::core::result<wh::compose::graph_value> {
                            return wh::compose::graph_value{20};
                          })
              .has_value());
  REQUIRE(graph
              .add_lambda(
                  "join",
                  [&merged_count](const wh::compose::graph_value &input, wh::core::run_context &,
                                  const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    auto merged =
                        wh::testing::helper::read_graph_value<wh::compose::graph_value_map>(input);
                    if (merged.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(merged.error());
                    }
                    merged_count = merged.value().size();
                    auto left = wh::testing::helper::read_graph_value<int>(merged.value().at("a"));
                    auto right = wh::testing::helper::read_graph_value<int>(merged.value().at("b"));
                    if (left.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(left.error());
                    }
                    if (right.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(right.error());
                    }
                    return wh::compose::graph_value{left.value() + right.value()};
                  })
              .has_value());
  REQUIRE(graph.add_entry_edge("a").has_value());
  REQUIRE(graph.add_entry_edge("b").has_value());
  REQUIRE(graph.add_edge("a", "join").has_value());
  REQUIRE(graph.add_edge("b", "join").has_value());
  REQUIRE(graph.add_exit_edge("join").has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context context{};
  auto invoked =
      wh::testing::helper::invoke_value_sync(graph, wh::compose::graph_value{0}, context);
  REQUIRE(invoked.has_value());
  auto typed = wh::testing::helper::read_graph_value<int>(std::move(invoked).value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == 30);
  REQUIRE(merged_count == 2U);
}

TEST_CASE("dag input lowers stream predecessors into collected value payloads only after eof",
          "[UT][wh/compose/graph/detail/"
          "dag_input.hpp][graph::lower_reader][condition][branch][boundary]") {
  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::dag;
  options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
  options.fan_in_policy = wh::compose::graph_fan_in_policy::require_all_sources;
  wh::compose::graph graph{std::move(options)};

  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream>(
                  "a",
                  [](const wh::compose::graph_value &, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_stream_reader> {
                    return wh::compose::make_single_value_stream_reader(std::string{"A"});
                  })
              .has_value());
  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream>(
                  "b",
                  [](const wh::compose::graph_value &, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_stream_reader> {
                    return wh::compose::make_single_value_stream_reader(std::string{"B"});
                  })
              .has_value());
  REQUIRE(graph
              .add_lambda(
                  "join",
                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    const auto collect_chunks = [](const wh::compose::graph_value &value)
                        -> wh::core::result<std::vector<std::string>> {
                      const auto *chunks =
                          wh::core::any_cast<std::vector<wh::compose::graph_value>>(&value);
                      if (chunks == nullptr) {
                        return wh::core::result<std::vector<std::string>>::failure(
                            wh::core::errc::type_mismatch);
                      }
                      std::vector<std::string> values{};
                      values.reserve(chunks->size());
                      for (const auto &chunk : *chunks) {
                        auto typed = wh::testing::helper::read_graph_value<std::string>(chunk);
                        if (typed.has_error()) {
                          return wh::core::result<std::vector<std::string>>::failure(typed.error());
                        }
                        values.push_back(typed.value());
                      }
                      return values;
                    };

                    auto merged =
                        wh::testing::helper::read_graph_value<wh::compose::graph_value_map>(input);
                    if (merged.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(merged.error());
                    }
                    auto left = collect_chunks(merged.value().at("a"));
                    auto right = collect_chunks(merged.value().at("b"));
                    if (left.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(left.error());
                    }
                    if (right.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(right.error());
                    }
                    return wh::compose::graph_value{left.value().front() + "+" +
                                                    right.value().front()};
                  })
              .has_value());
  REQUIRE(graph.add_entry_edge("a").has_value());
  REQUIRE(graph.add_entry_edge("b").has_value());
  REQUIRE(graph.add_edge("a", "join", wh::compose::edge_options{}).has_value());
  REQUIRE(graph.add_edge("b", "join", wh::compose::edge_options{}).has_value());
  REQUIRE(graph.add_exit_edge("join").has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context context{};
  auto invoked =
      wh::testing::helper::invoke_value_sync(graph, wh::compose::graph_value{0}, context);
  REQUIRE(invoked.has_value());
  auto typed = wh::testing::helper::read_graph_value<std::string>(std::move(invoked).value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == "A+B");
}

TEST_CASE("dag input preserves stream pre-state readers as live input lowering",
          "[UT][wh/compose/graph/detail/"
          "dag_input.hpp][graph::build_node_input_sender][stream_pre][boundary]") {
  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::dag;
  options.boundary = {
      .input = wh::compose::node_contract::stream,
      .output = wh::compose::node_contract::value,
  };
  wh::compose::graph graph{std::move(options)};

  wh::compose::graph_add_node_options worker_options{};
  worker_options.state.bind_stream_pre<wh::compose::graph_value>(
      [](const wh::compose::graph_state_cause &, wh::compose::graph_process_state &,
         wh::compose::graph_value &,
         wh::core::run_context &) -> wh::core::result<void> { return {}; });

  REQUIRE(graph
              .add_lambda(wh::compose::make_lambda_node(
                  "worker",
                  [](wh::compose::graph_value &value, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> { return std::move(value); },
                  worker_options))
              .has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  auto input_reader = wh::compose::make_single_value_stream_reader(std::string{"dag-stream"});
  REQUIRE(input_reader.has_value());

  wh::core::run_context context{};
  auto base = wh::testing::helper::make_invoke_session(
      graph, wh::compose::graph_value{std::move(input_reader).value()}, context);
  wh::compose::detail::invoke_runtime::dag_runtime dag_state{std::move(base)};
  dag_state.initialize_entry();

  auto worker_id = graph.node_id("worker");
  REQUIRE(worker_id.has_value());
  auto action = dag_state.take_ready_action();
  REQUIRE(action.kind == wh::compose::detail::invoke_runtime::ready_action_kind::launch);
  REQUIRE(action.attempt.has_value());
  REQUIRE(action.attempt.slot == worker_id.value());

  auto waited = stdexec::sync_wait(dag_state.make_input_sender(action.attempt));
  REQUIRE(waited.has_value());
  auto status = std::get<0>(std::move(*waited));
  REQUIRE(status.has_value());
  auto *reader = wh::core::any_cast<wh::compose::graph_stream_reader>(&status.value());
  REQUIRE(reader != nullptr);
  auto chunks = wh::compose::collect_graph_stream_reader(std::move(*reader));
  REQUIRE(chunks.has_value());
  REQUIRE(chunks->size() == 1U);
  auto typed = wh::testing::helper::read_graph_value<std::string>(chunks->front());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == "dag-stream");
}
