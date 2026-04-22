#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <exec/task.hpp>
#include <stdexec/execution.hpp>

#include "helper/static_thread_scheduler.hpp"
#include "wh/compose/graph/graph.hpp"
#include "wh/compose/node/lambda.hpp"

namespace {

template <typename value_t>
[[nodiscard]] auto read_any(wh::compose::graph_value &&value) -> wh::core::result<value_t> {
  if (auto *typed = wh::core::any_cast<value_t>(&value); typed != nullptr) {
    return std::move(*typed);
  }
  return wh::core::result<value_t>::failure(wh::core::errc::type_mismatch);
}

[[nodiscard]] auto await_graph_sender(wh::compose::graph_sender sender)
    -> wh::core::result<wh::compose::graph_value> {
  auto awaited = stdexec::sync_wait(std::move(sender));
  REQUIRE(awaited.has_value());
  return std::get<0>(std::move(*awaited));
}

[[nodiscard]] auto compile_lambda_node(const wh::compose::lambda_node &node)
    -> wh::core::result<wh::compose::compiled_node> {
  wh::compose::graph graph{};
  auto added = graph.add_lambda(node);
  if (added.has_error()) {
    return wh::core::result<wh::compose::compiled_node>::failure(added.error());
  }
  if (auto entry = graph.add_entry_edge(std::string{node.key()}); entry.has_error()) {
    return wh::core::result<wh::compose::compiled_node>::failure(entry.error());
  }
  if (auto exit = graph.add_exit_edge(std::string{node.key()}); exit.has_error()) {
    return wh::core::result<wh::compose::compiled_node>::failure(exit.error());
  }
  auto compiled = graph.compile();
  if (compiled.has_error()) {
    return wh::core::result<wh::compose::compiled_node>::failure(compiled.error());
  }
  auto lowered = graph.compiled_node_by_key(node.key());
  if (lowered.has_error()) {
    return wh::core::result<wh::compose::compiled_node>::failure(lowered.error());
  }
  return lowered.value().get();
}

using map_lambda_t =
    decltype([](wh::compose::graph_value_map &input, wh::core::run_context &,
                const wh::compose::graph_call_scope &)
                 -> wh::core::result<wh::compose::graph_value_map> { return input; });

using stream_lambda_t = decltype([](wh::compose::graph_stream_reader, wh::core::run_context &,
                                    const wh::compose::graph_call_scope &) {
  return wh::compose::make_single_value_stream_reader(0);
});

} // namespace

TEST_CASE("lambda helpers cover payload wrapping input adapters and gate deduction",
          "[UT][wh/compose/node/lambda.hpp][wrap_lambda_payload][condition][branch][boundary]") {
  wh::compose::graph_add_node_options options{};
  auto decorated = wh::compose::detail::decorate_lambda_options<map_lambda_t>(
      options, wh::compose::node_contract::value);
  REQUIRE_FALSE(decorated.type.empty());
  REQUIRE_FALSE(decorated.label.empty());

  auto wrapped_value = wh::compose::detail::wrap_lambda_payload(wh::core::result<int>{7});
  REQUIRE(wrapped_value.has_value());
  REQUIRE(*wh::core::any_cast<int>(&wrapped_value.value()) == 7);

  auto wrapped_error = wh::compose::detail::wrap_lambda_payload(
      wh::core::result<int>::failure(wh::core::errc::timeout));
  REQUIRE(wrapped_error.has_error());
  REQUIRE(wrapped_error.error() == wh::core::errc::timeout);

  auto reader = wh::compose::make_single_value_stream_reader(std::string{"chunk"});
  REQUIRE(reader.has_value());
  auto wrapped_stream = wh::compose::detail::wrap_lambda_stream_payload(std::move(reader));
  REQUIRE(wrapped_stream.has_value());
  auto restored_reader =
      read_any<wh::compose::graph_stream_reader>(std::move(wrapped_stream).value());
  REQUIRE(restored_reader.has_value());

  wh::compose::graph_value map_input{wh::compose::graph_value_map{
      {"x", wh::compose::graph_value{1}},
  }};
  auto map_read = wh::compose::detail::read_lambda_input<wh::compose::graph_value_map>(map_input);
  REQUIRE(map_read.has_value());
  REQUIRE(map_read.value().get().size() == 1U);

  wh::compose::graph_value wrong_input{9};
  auto wrong_map =
      wh::compose::detail::read_lambda_input<wh::compose::graph_value_map>(wrong_input);
  REQUIRE(wrong_map.has_error());
  REQUIRE(wrong_map.error() == wh::core::errc::type_mismatch);

  auto stream_source = wh::compose::make_single_value_stream_reader(2);
  REQUIRE(stream_source.has_value());
  wh::compose::graph_value stream_input{std::move(stream_source).value()};
  auto stream_read =
      wh::compose::detail::read_lambda_input<wh::compose::graph_stream_reader>(stream_input);
  REQUIRE(stream_read.has_value());

  REQUIRE(
      wh::compose::detail::lambda_input_gate<wh::compose::node_contract::value, map_lambda_t>() ==
      wh::compose::input_gate::exact<wh::compose::graph_value_map>());
  REQUIRE(wh::compose::detail::lambda_input_gate<wh::compose::node_contract::stream,
                                                 stream_lambda_t>() ==
          wh::compose::input_gate::reader());
  REQUIRE(wh::compose::detail::lambda_output_gate<wh::compose::node_contract::value, map_lambda_t>()
              .kind == wh::compose::output_gate_kind::value_exact);
  REQUIRE(
      wh::compose::detail::lambda_output_gate<wh::compose::node_contract::stream, map_lambda_t>() ==
      wh::compose::output_gate::reader());

  auto descriptor =
      wh::compose::detail::make_lambda_descriptor<wh::compose::node_contract::value,
                                                  wh::compose::node_contract::value, map_lambda_t>(
          "map", wh::compose::node_exec_mode::sync);
  REQUIRE(descriptor.key == "map");
  REQUIRE(descriptor.kind == wh::compose::node_kind::lambda);
  REQUIRE(descriptor.exec_mode == wh::compose::node_exec_mode::sync);
}

TEST_CASE("lambda node builders cover sync value map stream and async stream execution",
          "[UT][wh/compose/node/lambda.hpp][make_lambda_node][condition][branch]") {
  auto value_node = wh::compose::make_lambda_node(
      "value",
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
        auto *typed = wh::core::any_cast<int>(&input);
        REQUIRE(typed != nullptr);
        return wh::compose::graph_value{*typed + 3};
      });
  auto compiled_value = compile_lambda_node(value_node);
  REQUIRE(compiled_value.has_value());

  wh::compose::graph_value value_input{4};
  wh::core::run_context context{};
  auto value_status = wh::compose::run_compiled_sync_node(compiled_value.value(), value_input,
                                                          context, wh::compose::node_runtime{});
  REQUIRE(value_status.has_value());
  REQUIRE(*wh::core::any_cast<int>(&value_status.value()) == 7);

  auto map_node = wh::compose::make_lambda_node("map", map_lambda_t{});
  auto compiled_map = compile_lambda_node(map_node);
  REQUIRE(compiled_map.has_value());
  wh::compose::graph_value map_wrong{1};
  auto map_status = wh::compose::run_compiled_sync_node(compiled_map.value(), map_wrong, context,
                                                        wh::compose::node_runtime{});
  REQUIRE(map_status.has_error());
  REQUIRE(map_status.error() == wh::core::errc::type_mismatch);

  auto stream_value_node = wh::compose::make_lambda_node<wh::compose::node_contract::stream,
                                                         wh::compose::node_contract::value>(
      "collect",
      [](wh::compose::graph_stream_reader reader, wh::core::run_context &,
         const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
        auto collected = wh::compose::collect_graph_stream_reader(std::move(reader));
        if (collected.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(collected.error());
        }
        int sum = 0;
        for (auto &entry : collected.value()) {
          auto *typed = wh::core::any_cast<int>(&entry);
          REQUIRE(typed != nullptr);
          sum += *typed;
        }
        return wh::compose::graph_value{sum};
      });
  auto compiled_stream_value = compile_lambda_node(stream_value_node);
  REQUIRE(compiled_stream_value.has_value());
  auto numbers = wh::compose::make_values_stream_reader(std::vector<wh::compose::graph_value>{
      wh::compose::graph_value{1},
      wh::compose::graph_value{2},
  });
  REQUIRE(numbers.has_value());
  wh::compose::graph_value stream_value_input{std::move(numbers).value()};
  auto stream_value_status = wh::compose::run_compiled_sync_node(
      compiled_stream_value.value(), stream_value_input, context, wh::compose::node_runtime{});
  REQUIRE(stream_value_status.has_value());
  REQUIRE(*wh::core::any_cast<int>(&stream_value_status.value()) == 3);

  auto sync_stream_node = wh::compose::make_lambda_node<wh::compose::node_contract::value,
                                                        wh::compose::node_contract::stream>(
      "sync-stream",
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_stream_reader> {
        auto *typed = wh::core::any_cast<int>(&input);
        REQUIRE(typed != nullptr);
        return wh::compose::make_values_stream_reader(std::vector<wh::compose::graph_value>{
            wh::compose::graph_value{*typed},
            wh::compose::graph_value{*typed + 1},
        });
      });
  auto compiled_sync_stream = compile_lambda_node(sync_stream_node);
  REQUIRE(compiled_sync_stream.has_value());

  wh::compose::graph_value sync_stream_input{6};
  auto sync_stream_status = wh::compose::run_compiled_sync_node(
      compiled_sync_stream.value(), sync_stream_input, context, wh::compose::node_runtime{});
  REQUIRE(sync_stream_status.has_value());
  auto sync_stream_reader =
      read_any<wh::compose::graph_stream_reader>(std::move(sync_stream_status).value());
  REQUIRE(sync_stream_reader.has_value());
  auto sync_stream_chunks =
      wh::compose::collect_graph_stream_reader(std::move(sync_stream_reader).value());
  REQUIRE(sync_stream_chunks.has_value());
  REQUIRE(sync_stream_chunks.value().size() == 2U);
  REQUIRE(*wh::core::any_cast<int>(&sync_stream_chunks.value()[0]) == 6);
  REQUIRE(*wh::core::any_cast<int>(&sync_stream_chunks.value()[1]) == 7);
}

TEST_CASE("async value to stream lambda returns graph stream reader payload",
          "[UT][wh/compose/node/lambda.hpp][make_lambda_node][condition][branch][async]") {
  auto async_stream_node = wh::compose::make_lambda_node<wh::compose::node_contract::value,
                                                         wh::compose::node_contract::stream,
                                                         wh::compose::node_exec_mode::async>(
      "async-stream", [](wh::compose::graph_value &input, wh::core::run_context &,
                         const wh::compose::graph_call_scope &) {
        auto *typed = wh::core::any_cast<int>(&input);
        REQUIRE(typed != nullptr);
        return stdexec::just(wh::core::result<wh::compose::graph_stream_reader>{
            wh::compose::make_values_stream_reader(std::vector<wh::compose::graph_value>{
                                                       wh::compose::graph_value{*typed},
                                                       wh::compose::graph_value{*typed + 1},
                                                   })
                .value()});
      });
  auto compiled_async_stream = compile_lambda_node(async_stream_node);
  REQUIRE(compiled_async_stream.has_value());

  wh::testing::helper::static_thread_scheduler_helper scheduler_helper{1U};
  auto scheduler = wh::core::detail::erase_resume_scheduler(scheduler_helper.scheduler());
  wh::compose::node_runtime runtime{};
  runtime.set_control_scheduler(&scheduler);

  wh::compose::graph_value async_input{6};
  wh::core::run_context context{};
  auto async_status = await_graph_sender(wh::compose::run_compiled_async_node(
      compiled_async_stream.value(), async_input, context, runtime));
  REQUIRE(async_status.has_value());
  auto async_reader = read_any<wh::compose::graph_stream_reader>(std::move(async_status).value());
  REQUIRE(async_reader.has_value());
}

TEST_CASE("async lambda exec task senders compose through graph boundaries",
          "[UT][wh/compose/node/lambda.hpp][make_lambda_node][async][exec::task][graph]") {
  auto task_value_value = wh::compose::make_lambda_node<wh::compose::node_contract::value,
                                                        wh::compose::node_contract::value,
                                                        wh::compose::node_exec_mode::async>(
      "task-vv",
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> exec::task<wh::core::result<wh::compose::graph_value>> {
        auto *typed = wh::core::any_cast<int>(&input);
        REQUIRE(typed != nullptr);
        co_return wh::core::result<wh::compose::graph_value>{wh::compose::graph_value{*typed + 1}};
      });

  auto sync_value_stream = wh::compose::make_lambda_node<wh::compose::node_contract::value,
                                                         wh::compose::node_contract::stream>(
      "sync-vs",
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_stream_reader> {
        auto *typed = wh::core::any_cast<int>(&input);
        REQUIRE(typed != nullptr);
        return wh::compose::make_values_stream_reader(std::vector<wh::compose::graph_value>{
            wh::compose::graph_value{*typed},
            wh::compose::graph_value{*typed * 2},
        });
      });

  auto task_stream_value = wh::compose::make_lambda_node<wh::compose::node_contract::stream,
                                                         wh::compose::node_contract::value,
                                                         wh::compose::node_exec_mode::async>(
      "task-sv",
      [](wh::compose::graph_stream_reader reader, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> exec::task<wh::core::result<wh::compose::graph_value>> {
        auto chunks = wh::compose::collect_graph_stream_reader(std::move(reader));
        if (chunks.has_error()) {
          co_return wh::core::result<wh::compose::graph_value>::failure(chunks.error());
        }

        int total = 0;
        for (auto &chunk : chunks.value()) {
          auto typed = read_any<int>(std::move(chunk));
          if (typed.has_error()) {
            co_return wh::core::result<wh::compose::graph_value>::failure(typed.error());
          }
          total += typed.value();
        }
        co_return wh::core::result<wh::compose::graph_value>{wh::compose::graph_value{total}};
      });

  wh::compose::graph graph{};
  REQUIRE(graph.add_lambda(task_value_value).has_value());
  REQUIRE(graph.add_lambda(sync_value_stream).has_value());
  REQUIRE(graph.add_lambda(task_stream_value).has_value());

  REQUIRE(graph.add_entry_edge("task-vv").has_value());
  REQUIRE(graph.add_edge("task-vv", "sync-vs").has_value());
  REQUIRE(graph.add_edge("sync-vs", "task-sv").has_value());
  REQUIRE(graph.add_exit_edge("task-sv").has_value());
  REQUIRE(graph.compile().has_value());

  wh::compose::graph_invoke_request request{};
  request.input = wh::compose::graph_input::value(3);

  wh::core::run_context context{};
  auto waited = stdexec::sync_wait(graph.invoke(context, std::move(request)));
  REQUIRE(waited.has_value());

  auto status = std::get<0>(std::move(*waited));
  REQUIRE(status.has_value());
  REQUIRE(status.value().output_status.has_value());

  auto output = read_any<int>(std::move(status).value().output_status.value());
  REQUIRE(output.has_value());
  REQUIRE(output.value() == 12);
}

TEST_CASE("async lambda requires graph scheduler when compiled async nodes are invoked directly",
          "[UT][wh/compose/node/lambda.hpp][make_lambda_node][condition][error]") {
  auto async_value_node = wh::compose::make_lambda_node<wh::compose::node_contract::value,
                                                        wh::compose::node_contract::value,
                                                        wh::compose::node_exec_mode::async>(
      "async-value",
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> exec::task<wh::core::result<wh::compose::graph_value>> {
        auto *typed = wh::core::any_cast<int>(&input);
        REQUIRE(typed != nullptr);
        co_return wh::core::result<wh::compose::graph_value>{wh::compose::graph_value{*typed + 1}};
      });
  auto compiled = compile_lambda_node(async_value_node);
  REQUIRE(compiled.has_value());

  wh::compose::graph_value input{5};
  wh::core::run_context context{};
  auto waited = stdexec::sync_wait(wh::compose::run_compiled_async_node(
      compiled.value(), input, context, wh::compose::node_runtime{}));
  REQUIRE(waited.has_value());
  auto status = std::get<0>(std::move(*waited));
  REQUIRE(status.has_error());
  REQUIRE(status.error() == wh::core::errc::contract_violation);
}
