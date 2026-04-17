#include <catch2/catch_test_macros.hpp>

#include <tuple>

#include <stdexec/execution.hpp>

#include "helper/compose_graph_runtime_support.hpp"
#include "helper/compose_graph_test_utils.hpp"
#include "wh/compose/graph/detail/pregel.hpp"
#include "wh/compose/graph/detail/pregel_entry.hpp"
#include "wh/compose/graph/detail/pregel_input.hpp"
#include "wh/compose/graph/detail/pregel_loop.hpp"

TEST_CASE("pregel input aggregates predecessor values into one keyed payload per superstep",
          "[UT][wh/compose/graph/detail/pregel_input.hpp][graph::build_pregel_node_input_sender][condition][branch][boundary]") {
  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::pregel;
  options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
  options.max_steps = 8U;
  wh::compose::graph graph{std::move(options)};

  REQUIRE(graph.add_lambda(
              wh::testing::helper::make_int_add_node("a", 1)).has_value());
  REQUIRE(graph.add_lambda(
              wh::testing::helper::make_int_add_node("b", 2)).has_value());
  REQUIRE(graph
              .add_lambda(
                  "join",
                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    auto merged =
                        wh::testing::helper::read_graph_value<
                            wh::compose::graph_value_map>(input);
                    if (merged.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(
                          merged.error());
                    }
                    auto left = wh::testing::helper::read_graph_value<int>(
                        merged.value().at("a"));
                    auto right = wh::testing::helper::read_graph_value<int>(
                        merged.value().at("b"));
                    if (left.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(
                          left.error());
                    }
                    if (right.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(
                          right.error());
                    }
                    return wh::compose::graph_value{
                        left.value() + right.value() + 3};
                  })
              .has_value());
  REQUIRE(graph.add_entry_edge("a").has_value());
  REQUIRE(graph.add_entry_edge("b").has_value());
  REQUIRE(graph.add_edge("a", "join").has_value());
  REQUIRE(graph.add_edge("b", "join").has_value());
  REQUIRE(graph.add_exit_edge("join").has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context context{};
  wh::compose::graph_call_options call_options{};
  call_options.record_transition_log = true;

  auto invoked = wh::testing::helper::invoke_graph_sync(
      graph, wh::compose::graph_value{1}, context, std::move(call_options));
  REQUIRE(invoked.has_value());
  REQUIRE(invoked->output_status.has_value());
  auto typed =
      wh::testing::helper::read_graph_value<int>(std::move(invoked->output_status).value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == 8);
  REQUIRE_FALSE(invoked->report.transition_log.empty());
}

TEST_CASE("pregel input readiness marks control-free nodes ready when allow_no_control is enabled",
          "[UT][wh/compose/graph/detail/pregel_input.hpp][graph::classify_pregel_node_readiness][condition][branch][boundary]") {
  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::pregel;
  options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
  options.max_steps = 2U;
  wh::compose::graph graph{std::move(options)};

  wh::compose::graph_add_node_options node_options{};
  node_options.allow_no_control = true;
  REQUIRE(graph
              .add_lambda(wh::compose::make_lambda_node(
                  "worker",
                  [](const wh::compose::graph_value &, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    return wh::compose::graph_value{1};
                  },
                  node_options))
              .has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context context{};
  auto base = wh::testing::helper::make_invoke_session(
      graph, wh::compose::graph_value{0}, context);
  wh::compose::detail::invoke_runtime::pregel_runtime pregel_state{
      std::move(base)};
  pregel_state.initialize_entry();

  auto worker_id = graph.node_id("worker");
  REQUIRE(worker_id.has_value());

  const auto &frontier = pregel_state.pregel_delivery().current_frontier();
  REQUIRE(frontier.size() == 1U);
  REQUIRE(frontier.front() == worker_id.value());

  const auto action = pregel_state.take_ready_action(worker_id.value(), 1U);
  REQUIRE(action.action ==
          wh::compose::detail::invoke_runtime::pregel_action::kind::launch);
  REQUIRE(action.node_id == worker_id.value());
  REQUIRE(action.attempt.has_value());
  REQUIRE(action.attempt.slot == worker_id.value());
  REQUIRE(action.cause.node_key == "worker");
}

TEST_CASE("pregel input preserves stream pre-state readers as live input lowering",
          "[UT][wh/compose/graph/detail/pregel_input.hpp][graph::build_pregel_node_input_sender][stream_pre][boundary]") {
  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::pregel;
  options.boundary = {
      .input = wh::compose::node_contract::stream,
      .output = wh::compose::node_contract::value,
  };
  options.max_steps = 2U;
  wh::compose::graph graph{std::move(options)};

  wh::compose::graph_add_node_options worker_options{};
  worker_options.state.bind_stream_pre<wh::compose::graph_value>(
      [](const wh::compose::graph_state_cause &,
         wh::compose::graph_process_state &, wh::compose::graph_value &,
         wh::core::run_context &) -> wh::core::result<void> { return {}; });

  REQUIRE(graph
              .add_lambda(wh::compose::make_lambda_node(
                  "worker",
                  [](wh::compose::graph_value &value, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    return std::move(value);
                  },
                  worker_options))
              .has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  auto input_reader =
      wh::compose::make_single_value_stream_reader(std::string{"pregel-stream"});
  REQUIRE(input_reader.has_value());

  wh::core::run_context context{};
  auto base = wh::testing::helper::make_invoke_session(
      graph, wh::compose::graph_value{std::move(input_reader).value()},
      context);
  wh::compose::detail::invoke_runtime::pregel_runtime pregel_state{
      std::move(base)};
  pregel_state.initialize_entry();

  auto worker_id = graph.node_id("worker");
  REQUIRE(worker_id.has_value());
  auto action = pregel_state.take_ready_action(worker_id.value(), 1U);
  REQUIRE(action.action ==
          wh::compose::detail::invoke_runtime::pregel_action::kind::launch);
  REQUIRE(action.attempt.has_value());
  REQUIRE(action.attempt.slot == worker_id.value());

  auto waited =
      stdexec::sync_wait(pregel_state.make_input_sender(action.attempt));
  REQUIRE(waited.has_value());
  auto status = std::get<0>(std::move(*waited));
  REQUIRE(status.has_value());
  auto *reader =
      wh::core::any_cast<wh::compose::graph_stream_reader>(&status.value());
  REQUIRE(reader != nullptr);
  auto chunks = wh::compose::collect_graph_stream_reader(std::move(*reader));
  REQUIRE(chunks.has_value());
  REQUIRE(chunks->size() == 1U);
  auto typed =
      wh::testing::helper::read_graph_value<std::string>(chunks->front());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == "pregel-stream");
}
