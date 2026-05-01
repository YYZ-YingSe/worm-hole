#include <tuple>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "wh/compose/graph.hpp"
#include "wh/compose/graph/detail/inline_impl.hpp"
#include "wh/compose/graph/detail/state_phase.hpp"
#include "wh/compose/node.hpp"
#include "wh/compose/node/lambda.hpp"

namespace {

[[nodiscard]] auto await_value(wh::compose::graph &graph, wh::compose::graph_value input)
    -> wh::core::result<wh::compose::graph_value> {
  wh::compose::graph_invoke_request request{};
  request.input = wh::compose::graph_input::value(std::move(input));
  wh::core::run_context context{};
  auto waited = stdexec::sync_wait(graph.invoke(context, std::move(request)));
  REQUIRE(waited.has_value());
  auto invoke_status = std::get<0>(std::move(*waited));
  REQUIRE(invoke_status.has_value());
  return std::move(invoke_status).value().output_status;
}

} // namespace

TEST_CASE("state phase helpers apply value and stream handlers through public graph invoke paths",
          "[UT][wh/compose/graph/detail/"
          "state_phase.hpp][graph::apply_state_phase_async][condition][branch][boundary]") {
  wh::compose::graph value_graph{};
  wh::compose::graph_add_node_options value_options{};
  value_options.state.bind_pre<wh::compose::graph_value>(
      [](const wh::compose::graph_state_cause &, wh::compose::graph_process_state &,
         wh::compose::graph_value &value, wh::core::run_context &) -> wh::core::result<void> {
        auto *typed = wh::core::any_cast<int>(&value);
        REQUIRE(typed != nullptr);
        *typed += 1;
        return {};
      });
  value_options.state.bind_post<wh::compose::graph_value>(
      [](const wh::compose::graph_state_cause &, wh::compose::graph_process_state &,
         wh::compose::graph_value &value, wh::core::run_context &) -> wh::core::result<void> {
        auto *typed = wh::core::any_cast<int>(&value);
        REQUIRE(typed != nullptr);
        *typed *= 2;
        return {};
      });
  auto add_value_worker = value_graph.add_lambda(wh::compose::make_lambda_node(
      "worker",
      [](wh::compose::graph_value &current_value, wh::core::run_context &,
         const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
        auto *typed = wh::core::any_cast<int>(&current_value);
        REQUIRE(typed != nullptr);
        return wh::compose::graph_value{*typed + 3};
      },
      value_options));
  REQUIRE(add_value_worker.has_value());
  REQUIRE(value_graph.add_entry_edge("worker").has_value());
  REQUIRE(value_graph.add_exit_edge("worker").has_value());
  REQUIRE(value_graph.compile().has_value());

  auto value_status = await_value(value_graph, wh::compose::graph_value{4});
  REQUIRE(value_status.has_value());
  REQUIRE(value_status->has_value());
  auto *output_value = wh::core::any_cast<int>(&value_status.value());
  REQUIRE(output_value != nullptr);
  REQUIRE(*output_value == 16);

  wh::compose::graph stream_graph{wh::compose::graph_boundary{
      .input = wh::compose::node_contract::value,
      .output = wh::compose::node_contract::stream,
  }};
  wh::compose::graph_add_node_options stream_options{};
  stream_options.state.bind_stream_post<wh::compose::graph_value>(
      [](const wh::compose::graph_state_cause &, wh::compose::graph_process_state &,
         wh::compose::graph_value &chunk, wh::core::run_context &) -> wh::core::result<void> {
        auto *typed = wh::core::any_cast<std::string>(&chunk);
        REQUIRE(typed != nullptr);
        typed->append("!");
        return {};
      });
  auto add_streamer =
      stream_graph.add_lambda(wh::compose::make_lambda_node<wh::compose::node_contract::value,
                                                            wh::compose::node_contract::stream>(
          "streamer",
          [](wh::compose::graph_value &current_value, wh::core::run_context &,
             const wh::compose::graph_call_scope &)
              -> wh::core::result<wh::compose::graph_stream_reader> {
            auto *typed = wh::core::any_cast<int>(&current_value);
            REQUIRE(typed != nullptr);
            return wh::compose::make_single_value_stream_reader(std::to_string(*typed));
          },
          stream_options));
  REQUIRE(add_streamer.has_value());
  REQUIRE(stream_graph.add_entry_edge("streamer").has_value());
  REQUIRE(stream_graph.add_exit_edge("streamer").has_value());
  REQUIRE(stream_graph.compile().has_value());

  auto stream_status = await_value(stream_graph, wh::compose::graph_value{7});
  REQUIRE(stream_status.has_value());
  REQUIRE(stream_status->has_value());
  auto *reader = wh::core::any_cast<wh::compose::graph_stream_reader>(&stream_status.value());
  REQUIRE(reader != nullptr);
  auto chunks = wh::compose::collect_graph_stream_reader(std::move(*reader));
  REQUIRE(chunks.has_value());
  REQUIRE(chunks->size() == 1U);
  auto *chunk = wh::core::any_cast<std::string>(&chunks->front());
  REQUIRE(chunk != nullptr);
  REQUIRE(*chunk == "7!");
}

TEST_CASE("state phase helpers surface handler failures through invoke output status",
          "[UT][wh/compose/graph/detail/"
          "state_phase.hpp][graph::apply_state_phase][condition][branch][boundary]") {
  wh::compose::graph graph{};
  wh::compose::graph_add_node_options options{};
  options.state.bind_pre<wh::compose::graph_value>(
      [](const wh::compose::graph_state_cause &, wh::compose::graph_process_state &,
         wh::compose::graph_value &, wh::core::run_context &) -> wh::core::result<void> {
        return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
      });

  auto add_failure_worker = graph.add_lambda(wh::compose::make_lambda_node(
      "worker",
      [](wh::compose::graph_value &current_value, wh::core::run_context &,
         const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
        return std::move(current_value);
      },
      options));
  REQUIRE(add_failure_worker.has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  auto status = await_value(graph, wh::compose::graph_value{5});
  REQUIRE(status.has_error());
  REQUIRE(status.error() == wh::core::errc::invalid_argument);
}
