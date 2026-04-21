#include <string>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include "helper/compose_graph_test_utils.hpp"
#include "wh/compose/node.hpp"

namespace {

using wh::testing::helper::build_single_node_graph;
using wh::testing::helper::execute_single_compiled_node;
using wh::testing::helper::invoke_value_sync;
using wh::testing::helper::read_graph_value;

} // namespace

TEST_CASE("compose lambda node adapts function and sender nodes",
          "[core][compose][node][functional]") {
  wh::compose::graph graph{};
  REQUIRE(graph
              .add_lambda(
                  "fn",
                  [](wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> { return std::move(input); })
              .has_value());
  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::value,
                          wh::compose::node_exec_mode::async>(
                  "sender",
                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &) -> decltype(auto) {
                    return stdexec::just(wh::core::result<wh::compose::graph_value>{input});
                  })
              .has_value());
  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::value,
                          wh::compose::node_exec_mode::async>(
                  "sender_tail",
                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &) -> decltype(auto) {
                    return stdexec::just(wh::core::result<wh::compose::graph_value>{input});
                  })
              .has_value());
  REQUIRE(graph.add_entry_edge("fn").has_value());
  REQUIRE(graph.add_edge("fn", "sender").has_value());
  REQUIRE(graph.add_edge("sender", "sender_tail").has_value());
  REQUIRE(graph.add_exit_edge("sender_tail").has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context context{};
  auto invoked = invoke_value_sync(graph, wh::core::any(7), context);
  REQUIRE(invoked.has_value());
  auto typed = read_graph_value<int>(invoked.value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == 7);
}

TEST_CASE("compose lambda node writes metadata and supports transform path",
          "[core][compose][lambda][condition]") {
  auto invoke_node = wh::compose::make_lambda_node(
      "lambda-invoke",
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
        return std::move(input);
      });
  REQUIRE(!invoke_node.mutable_options().type.empty());
  REQUIRE(invoke_node.mutable_options().input_key.empty());
  REQUIRE(invoke_node.mutable_options().output_key.empty());
  REQUIRE(!invoke_node.mutable_options().label.empty());

  auto transform_node = wh::compose::make_lambda_node(
      "lambda-transform",
      [](wh::compose::graph_value_map &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value_map> {
        return std::move(input);
      });
  REQUIRE(!transform_node.mutable_options().type.empty());
  REQUIRE(transform_node.mutable_options().input_key.empty());
  REQUIRE(transform_node.mutable_options().output_key.empty());
}

TEST_CASE("compose lambda node supports function and sender invoke adapters",
          "[core][compose][lambda][branch]") {
  auto function_node = wh::compose::make_lambda_node(
      "lambda-function",
      [](const wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
        auto typed = read_graph_value<int>(input);
        if (typed.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(typed.error());
        }
        return wh::core::any(typed.value() + 1);
      });

  auto sender_node = wh::compose::make_lambda_node<wh::compose::node_contract::value,
                                                   wh::compose::node_contract::value,
                                                   wh::compose::node_exec_mode::async>(
      "lambda-sender", [](const wh::compose::graph_value &input, wh::core::run_context &,
                          const wh::compose::graph_call_scope &) {
        auto typed = read_graph_value<int>(input);
        if (typed.has_error()) {
          return stdexec::just(wh::core::result<wh::compose::graph_value>::failure(typed.error()));
        }
        return stdexec::just(
            wh::core::result<wh::compose::graph_value>{wh::core::any(typed.value() + 2)});
      });

  wh::core::run_context context{};
  auto function_output = execute_single_compiled_node(function_node, wh::core::any(5), context);
  REQUIRE(function_output.has_value());
  auto function_typed = read_graph_value<int>(function_output.value());
  REQUIRE(function_typed.has_value());
  REQUIRE(function_typed.value() == 6);

  auto sender_output = execute_single_compiled_node(sender_node, wh::core::any(5), context);
  REQUIRE(sender_output.has_value());
  auto sender_typed = read_graph_value<int>(sender_output.value());
  REQUIRE(sender_typed.has_value());
  REQUIRE(sender_typed.value() == 7);
}

TEST_CASE("compose lambda node supports function and sender transform adapters",
          "[core][compose][lambda][branch]") {
  auto function_node = wh::compose::make_lambda_node(
      "lambda-transform-function",
      [](const wh::compose::graph_value_map &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value_map> {
        wh::compose::graph_value_map output = input;
        output.insert_or_assign("task", wh::core::any(1));
        return output;
      });

  auto sender_node = wh::compose::make_lambda_node<wh::compose::node_contract::value,
                                                   wh::compose::node_contract::value,
                                                   wh::compose::node_exec_mode::async>(
      "lambda-transform-sender",
      [](const wh::compose::graph_value_map &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &) {
        wh::compose::graph_value_map output = input;
        output.insert_or_assign("sender", wh::core::any(2));
        return stdexec::just(wh::core::result<wh::compose::graph_value_map>{std::move(output)});
      });

  auto make_payload = [] {
    wh::compose::graph_value_map input{};
    input.insert_or_assign("base", wh::core::any(0));
    return wh::core::any(std::move(input));
  };

  wh::core::run_context context{};
  auto function_output = execute_single_compiled_node(function_node, make_payload(), context);
  REQUIRE(function_output.has_value());
  auto function_typed = read_graph_value<wh::compose::graph_value_map>(function_output.value());
  REQUIRE(function_typed.has_value());
  REQUIRE(function_typed.value().contains("base"));
  REQUIRE(function_typed.value().contains("task"));

  auto sender_output = execute_single_compiled_node(sender_node, make_payload(), context);
  REQUIRE(sender_output.has_value());
  auto sender_typed = read_graph_value<wh::compose::graph_value_map>(sender_output.value());
  REQUIRE(sender_typed.has_value());
  REQUIRE(sender_typed.value().contains("base"));
  REQUIRE(sender_typed.value().contains("sender"));
}

TEST_CASE("compose lambda node supports explicit boundary matrix",
          "[core][compose][lambda][boundary]") {
  auto value_to_stream = wh::compose::make_lambda_node<wh::compose::node_contract::value,
                                                       wh::compose::node_contract::stream>(
      "value-to-stream",
      [](const wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_stream_reader> {
        auto typed = read_graph_value<int>(input);
        if (typed.has_error()) {
          return wh::core::result<wh::compose::graph_stream_reader>::failure(typed.error());
        }
        return wh::compose::make_single_value_stream_reader(typed.value() + 1);
      });
  auto lowered_value_to_stream = build_single_node_graph(value_to_stream);
  REQUIRE(lowered_value_to_stream.has_value());
  REQUIRE(lowered_value_to_stream->node->meta.input_contract == wh::compose::node_contract::value);
  REQUIRE(lowered_value_to_stream->node->meta.output_contract ==
          wh::compose::node_contract::stream);

  auto stream_to_value = wh::compose::make_lambda_node<wh::compose::node_contract::stream,
                                                       wh::compose::node_contract::value>(
      "stream-to-value",
      [](wh::compose::graph_stream_reader input, wh::core::run_context &,
         const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
        auto chunks = wh::compose::collect_graph_stream_reader(std::move(input));
        if (chunks.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(chunks.error());
        }
        int sum = 0;
        for (const auto &chunk : chunks.value()) {
          auto typed = read_graph_value<int>(chunk);
          if (typed.has_error()) {
            return wh::core::result<wh::compose::graph_value>::failure(typed.error());
          }
          sum += typed.value();
        }
        return wh::core::any(sum);
      });
  auto lowered_stream_to_value = build_single_node_graph(stream_to_value);
  REQUIRE(lowered_stream_to_value.has_value());
  REQUIRE(lowered_stream_to_value->node->meta.input_contract == wh::compose::node_contract::stream);
  REQUIRE(lowered_stream_to_value->node->meta.output_contract == wh::compose::node_contract::value);

  auto stream_to_stream = wh::compose::make_lambda_node<wh::compose::node_contract::stream,
                                                        wh::compose::node_contract::stream>(
      "stream-to-stream",
      [](wh::compose::graph_stream_reader input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_stream_reader> {
        auto chunks = wh::compose::collect_graph_stream_reader(std::move(input));
        if (chunks.has_error()) {
          return wh::core::result<wh::compose::graph_stream_reader>::failure(chunks.error());
        }
        int sum = 0;
        for (const auto &chunk : chunks.value()) {
          auto typed = read_graph_value<int>(chunk);
          if (typed.has_error()) {
            return wh::core::result<wh::compose::graph_stream_reader>::failure(typed.error());
          }
          sum += typed.value();
        }
        return wh::compose::make_single_value_stream_reader(sum);
      });
  auto lowered_stream_to_stream = build_single_node_graph(stream_to_stream);
  REQUIRE(lowered_stream_to_stream.has_value());
  REQUIRE(lowered_stream_to_stream->node->meta.input_contract ==
          wh::compose::node_contract::stream);
  REQUIRE(lowered_stream_to_stream->node->meta.output_contract ==
          wh::compose::node_contract::stream);

  wh::core::run_context context{};
  auto value_to_stream_output =
      execute_single_compiled_node(value_to_stream, wh::core::any(5), context);
  REQUIRE(value_to_stream_output.has_value());
  auto value_to_stream_reader =
      read_graph_value<wh::compose::graph_stream_reader>(std::move(value_to_stream_output).value());
  REQUIRE(value_to_stream_reader.has_value());
  auto value_to_stream_chunks =
      wh::compose::collect_graph_stream_reader(std::move(value_to_stream_reader).value());
  REQUIRE(value_to_stream_chunks.has_value());
  REQUIRE(value_to_stream_chunks.value().size() == 1U);
  auto lifted_value = read_graph_value<int>(value_to_stream_chunks.value()[0]);
  REQUIRE(lifted_value.has_value());
  REQUIRE(lifted_value.value() == 6);

  auto [stream_to_value_writer, stream_to_value_reader] = wh::compose::make_graph_stream();
  REQUIRE(stream_to_value_writer.try_write(wh::core::any(2)).has_value());
  REQUIRE(stream_to_value_writer.try_write(wh::core::any(3)).has_value());
  REQUIRE(stream_to_value_writer.close().has_value());
  auto stream_to_value_output = execute_single_compiled_node(
      stream_to_value, wh::core::any(std::move(stream_to_value_reader)), context);
  REQUIRE(stream_to_value_output.has_value());
  auto collected_value = read_graph_value<int>(stream_to_value_output.value());
  REQUIRE(collected_value.has_value());
  REQUIRE(collected_value.value() == 5);

  auto [stream_to_stream_writer, stream_to_stream_reader] = wh::compose::make_graph_stream();
  REQUIRE(stream_to_stream_writer.try_write(wh::core::any(4)).has_value());
  REQUIRE(stream_to_stream_writer.try_write(wh::core::any(5)).has_value());
  REQUIRE(stream_to_stream_writer.close().has_value());
  auto stream_to_stream_output = execute_single_compiled_node(
      stream_to_stream, wh::core::any(std::move(stream_to_stream_reader)), context);
  REQUIRE(stream_to_stream_output.has_value());
  auto stream_to_stream_result = read_graph_value<wh::compose::graph_stream_reader>(
      std::move(stream_to_stream_output).value());
  REQUIRE(stream_to_stream_result.has_value());
  auto stream_to_stream_chunks =
      wh::compose::collect_graph_stream_reader(std::move(stream_to_stream_result).value());
  REQUIRE(stream_to_stream_chunks.has_value());
  REQUIRE(stream_to_stream_chunks.value().size() == 1U);
  auto transformed_value = read_graph_value<int>(stream_to_stream_chunks.value()[0]);
  REQUIRE(transformed_value.has_value());
  REQUIRE(transformed_value.value() == 9);
}

TEST_CASE("compose async value-to-stream lambda should preserve graph stream reader payload",
          "[core][compose][lambda][async][stream]") {
  auto async_value_to_stream = wh::compose::make_lambda_node<wh::compose::node_contract::value,
                                                             wh::compose::node_contract::stream,
                                                             wh::compose::node_exec_mode::async>(
      "async-value-to-stream", [](wh::compose::graph_value &input, wh::core::run_context &,
                                  const wh::compose::graph_call_scope &) {
        auto typed = read_graph_value<int>(input);
        if (typed.has_error()) {
          return stdexec::just(
              wh::core::result<wh::compose::graph_stream_reader>::failure(typed.error()));
        }
        auto reader = wh::compose::make_single_value_stream_reader(typed.value() + 1);
        if (reader.has_error()) {
          return stdexec::just(
              wh::core::result<wh::compose::graph_stream_reader>::failure(reader.error()));
        }
        return stdexec::just(
            wh::core::result<wh::compose::graph_stream_reader>{std::move(reader).value()});
      });

  auto compiled = build_single_node_graph(async_value_to_stream);
  REQUIRE(compiled.has_value());

  exec::static_thread_pool pool{1U};
  auto graph_scheduler = wh::core::detail::erase_resume_scheduler(pool.get_scheduler());
  wh::compose::node_runtime runtime{};
  runtime.set_control_scheduler(&graph_scheduler);

  wh::compose::graph_value input = wh::core::any(5);
  wh::core::run_context context{};
  auto waited = stdexec::sync_wait(
      wh::compose::run_compiled_async_node(*compiled->node, input, context, runtime));
  REQUIRE(waited.has_value());
  auto status = std::get<0>(std::move(waited).value());
  REQUIRE(status.has_value());
  INFO(status.value().info().name);
  auto reader = read_graph_value<wh::compose::graph_stream_reader>(std::move(status).value());
  REQUIRE(reader.has_value());
}
