#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include "helper/async_coordination.hpp"
#include "helper/compose_graph_runtime_support.hpp"
#include "helper/compose_graph_test_utils.hpp"
#include "helper/sender_capture.hpp"
#include "helper/static_thread_scheduler.hpp"
#include "wh/compose/graph/compile_options.hpp"
#include "wh/compose/graph/detail/build.hpp"
#include "wh/compose/graph/detail/child_pump.hpp"
#include "wh/compose/graph/detail/collect_policy.hpp"
#include "wh/compose/graph/detail/compile.hpp"
#include "wh/compose/graph/detail/graph_class.hpp"
#include "wh/compose/graph/detail/invoke.hpp"
#include "wh/compose/graph/invoke_types.hpp"
#include "wh/compose/graph/stream.hpp"
#include "wh/compose/node/component.hpp"
#include "wh/compose/node/detail/runtime_access.hpp"
#include "wh/compose/node/execution.hpp"
#include "wh/compose/node/lambda.hpp"
#include "wh/compose/node/passthrough.hpp"
#include "wh/compose/node/subgraph.hpp"
#include "wh/compose/node/tools.hpp"
#include "wh/compose/node/tools_contract.hpp"
#include "wh/compose/types.hpp"
#include "wh/core/any.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"
#include "wh/core/stdexec/ready_result_sender.hpp"
#include "wh/core/stdexec/resume_scheduler.hpp"
#include "wh/core/stdexec/result_sender.hpp"
#include "wh/document/document.hpp"
#include "wh/document/parser/text_parser.hpp"
#include "wh/document/processor.hpp"
#include "wh/embedding/embedding.hpp"
#include "wh/indexer/indexer.hpp"
#include "wh/model/echo_chat_model.hpp"
#include "wh/prompt/simple_chat_template.hpp"
#include "wh/retriever/retriever.hpp"
#include "wh/schema/message/parser.hpp"
#include "wh/schema/message/types.hpp"
#include "wh/schema/stream/pipe.hpp"
#include "wh/schema/stream/reader/values_stream_reader.hpp"
#include "wh/tool/tool.hpp"

namespace {

using graph_stream_result_sender = wh::compose::tools_stream_sender;
using synchronized_scheduler_helper = wh::testing::helper::static_thread_scheduler_helper;
using wh::testing::helper::capture_exact_checkpoint;

[[nodiscard]] auto make_int_graph_stream(std::initializer_list<int> values)
    -> wh::core::result<wh::compose::graph_stream_reader>;

[[nodiscard]] auto sum_collected_graph_chunks(const wh::compose::graph_value &input)
    -> wh::core::result<int>;

template <typename value_t>
[[nodiscard]] auto read_any(const wh::core::any &value) -> wh::core::result<value_t> {
  if (const auto *typed = wh::core::any_cast<value_t>(&value); typed != nullptr) {
    if constexpr (std::copy_constructible<value_t>) {
      return *typed;
    } else {
      return wh::core::result<value_t>::failure(wh::core::errc::not_supported);
    }
  }
  return wh::core::result<value_t>::failure(wh::core::errc::type_mismatch);
}

template <typename value_t>
[[nodiscard]] auto read_any(wh::core::any &&value) -> wh::core::result<value_t> {
  if (auto *typed = wh::core::any_cast<value_t>(&value); typed != nullptr) {
    return std::move(*typed);
  }
  return wh::core::result<value_t>::failure(wh::core::errc::type_mismatch);
}

struct collected_sum_consumer {
  auto invoke(std::vector<wh::compose::graph_value> input, wh::core::run_context &) const
      -> wh::core::result<int> {
    int sum = 0;
    for (auto &entry : input) {
      auto typed = read_any<int>(std::move(entry));
      if (typed.has_error()) {
        return wh::core::result<int>::failure(typed.error());
      }
      sum += typed.value();
    }
    return sum;
  }
};

template <typename result_t, stdexec::sender sender_t>
[[nodiscard]] auto wait_sender_result(sender_t sender) -> result_t {
  constexpr auto timeout = std::chrono::seconds{30};
  wh::testing::helper::sender_capture<result_t> capture{};
  auto env = wh::testing::helper::make_scheduler_env(stdexec::inline_scheduler{});
  auto operation = stdexec::connect(
      std::move(sender),
      wh::testing::helper::sender_capture_receiver<result_t, decltype(env)>{&capture, env});
  stdexec::start(operation);
  if (!capture.ready.try_acquire_for(timeout)) {
    return result_t::failure(wh::core::errc::timeout);
  }
  if (capture.terminal != wh::testing::helper::sender_terminal_kind::value ||
      !capture.value.has_value()) {
    return result_t::failure(wh::core::errc::canceled);
  }
  return std::move(*capture.value);
}

template <typename input_t>
[[nodiscard]] auto make_graph_request(input_t &&input) -> wh::compose::graph_invoke_request {
  wh::compose::graph_invoke_request request{};
  request.input = wh::compose::graph_input::value(std::forward<input_t>(input));
  return request;
}

[[nodiscard]] auto make_graph_request(wh::compose::graph_input input)
    -> wh::compose::graph_invoke_request {
  wh::compose::graph_invoke_request request{};
  request.input = std::move(input);
  return request;
}

[[nodiscard]] auto make_graph_request(wh::compose::graph_value input)
    -> wh::compose::graph_invoke_request {
  wh::compose::graph_invoke_request request{};
  if (auto *reader = wh::core::any_cast<wh::compose::graph_stream_reader>(&input);
      reader != nullptr) {
    request.input = wh::compose::graph_input::stream(std::move(*reader));
  } else {
    request.input = wh::compose::graph_input::value(std::move(input));
  }
  return request;
}

template <typename input_t>
[[nodiscard]] auto make_graph_request(input_t &&input,
                                      const wh::compose::graph_call_options &options)
    -> wh::compose::graph_invoke_request {
  auto request = make_graph_request(std::forward<input_t>(input));
  request.controls.call = options;
  return request;
}

template <typename input_t>
[[nodiscard]] auto make_graph_request(input_t &&input, wh::compose::graph_invoke_controls controls,
                                      const wh::compose::graph_runtime_services *services = nullptr)
    -> wh::compose::graph_invoke_request {
  auto request = make_graph_request(std::forward<input_t>(input));
  request.controls = std::move(controls);
  request.services = services;
  return request;
}

template <typename input_t>
[[nodiscard]] auto make_graph_request(input_t &&input,
                                      const wh::compose::graph_call_options &options,
                                      wh::compose::graph_invoke_controls controls,
                                      const wh::compose::graph_runtime_services *services = nullptr)
    -> wh::compose::graph_invoke_request {
  auto request = make_graph_request(std::forward<input_t>(input), std::move(controls), services);
  request.controls.call = options;
  return request;
}

[[nodiscard]] auto
unwrap_graph_invoke_status(wh::core::result<wh::compose::graph_invoke_result> status)
    -> wh::core::result<wh::compose::graph_value> {
  if (status.has_error()) {
    return wh::core::result<wh::compose::graph_value>::failure(status.error());
  }
  return std::move(status).value().output_status;
}

template <typename invokable_t, typename input_t>
concept sender_request_invokable =
    requires(const invokable_t &invokable, wh::core::run_context &context, input_t &&input) {
      invokable.invoke(context, make_graph_request(std::forward<input_t>(input)));
    };

template <typename invokable_t, typename input_t>
concept sync_value_invokable =
    requires(const invokable_t &invokable, wh::core::run_context &context, input_t &&input) {
      invokable.invoke(std::forward<input_t>(input), context);
    };

template <typename invokable_t, typename input_t>
concept sender_request_invokable_with_const_options =
    requires(const invokable_t &invokable, wh::core::run_context &context, input_t &&input,
             const wh::compose::graph_call_options &call_options) {
      invokable.invoke(context, make_graph_request(std::forward<input_t>(input), call_options));
    };

template <typename invokable_t, typename input_t>
concept sender_request_invokable_with_movable_options =
    requires(const invokable_t &invokable, wh::core::run_context &context, input_t &&input,
             wh::compose::graph_call_options &&call_options) {
      invokable.invoke(context, make_graph_request(std::forward<input_t>(input), call_options));
    };

template <typename invokable_t, typename input_t>
  requires sender_request_invokable<invokable_t, input_t>
[[nodiscard]] auto invoke_graph_sync(const invokable_t &invokable, input_t &&input,
                                     wh::core::run_context &context)
    -> wh::core::result<wh::compose::graph_invoke_result> {
  auto sender = invokable.invoke(context, make_graph_request(std::forward<input_t>(input)));
  return wait_sender_result<wh::core::result<wh::compose::graph_invoke_result>>(std::move(sender));
}

template <typename invokable_t, typename input_t>
  requires sender_request_invokable<invokable_t, input_t>
[[nodiscard]] auto invoke_graph_sync(const invokable_t &invokable, input_t &&input,
                                     wh::core::run_context &context,
                                     wh::compose::graph_invoke_controls controls,
                                     const wh::compose::graph_runtime_services *services = nullptr)
    -> wh::core::result<wh::compose::graph_invoke_result> {
  auto sender = invokable.invoke(
      context, make_graph_request(std::forward<input_t>(input), std::move(controls), services));
  return wait_sender_result<wh::core::result<wh::compose::graph_invoke_result>>(std::move(sender));
}

template <typename invokable_t, typename input_t>
  requires sender_request_invokable<invokable_t, input_t>
[[nodiscard]] auto invoke_graph_sync(const invokable_t &invokable, input_t &&input,
                                     wh::core::run_context &context,
                                     const wh::compose::graph_call_options &call_options,
                                     wh::compose::graph_invoke_controls controls,
                                     const wh::compose::graph_runtime_services *services = nullptr)
    -> wh::core::result<wh::compose::graph_invoke_result> {
  auto sender =
      invokable.invoke(context, make_graph_request(std::forward<input_t>(input), call_options,
                                                   std::move(controls), services));
  return wait_sender_result<wh::core::result<wh::compose::graph_invoke_result>>(std::move(sender));
}

template <typename invokable_t, typename input_t>
  requires sender_request_invokable<invokable_t, input_t>
[[nodiscard]] auto invoke_value_sync(const invokable_t &invokable, input_t &&input,
                                     wh::core::run_context &context)
    -> wh::core::result<wh::compose::graph_value> {
  auto sender = invokable.invoke(context, make_graph_request(std::forward<input_t>(input)));
  return unwrap_graph_invoke_status(
      wait_sender_result<wh::core::result<wh::compose::graph_invoke_result>>(std::move(sender)));
}

template <typename invokable_t, typename input_t>
  requires(!sender_request_invokable<invokable_t, input_t> &&
           sync_value_invokable<invokable_t, input_t>)
[[nodiscard]] auto invoke_value_sync(const invokable_t &invokable, input_t &&input,
                                     wh::core::run_context &context)
    -> wh::core::result<wh::compose::graph_value> {
  return invokable.invoke(std::forward<input_t>(input), context);
}

template <typename invokable_t, typename input_t>
  requires sender_request_invokable_with_const_options<invokable_t, input_t>
[[nodiscard]] auto invoke_value_sync(const invokable_t &invokable, input_t &&input,
                                     wh::core::run_context &context,
                                     const wh::compose::graph_call_options &call_options)
    -> wh::core::result<wh::compose::graph_value> {
  auto sender =
      invokable.invoke(context, make_graph_request(std::forward<input_t>(input), call_options));
  return unwrap_graph_invoke_status(
      wait_sender_result<wh::core::result<wh::compose::graph_invoke_result>>(std::move(sender)));
}

template <typename invokable_t, typename input_t>
  requires(!sender_request_invokable_with_const_options<invokable_t, input_t>)
[[nodiscard]] auto invoke_value_sync(const invokable_t &invokable, input_t &&input,
                                     wh::core::run_context &context,
                                     const wh::compose::graph_call_options &)
    -> wh::core::result<wh::compose::graph_value> {
  return invoke_value_sync(invokable, std::forward<input_t>(input), context);
}

template <typename invokable_t, typename input_t>
  requires sender_request_invokable_with_movable_options<invokable_t, input_t>
[[nodiscard]] auto invoke_value_sync(const invokable_t &invokable, input_t &&input,
                                     wh::core::run_context &context,
                                     wh::compose::graph_call_options &&call_options)
    -> wh::core::result<wh::compose::graph_value> {
  auto sender =
      invokable.invoke(context, make_graph_request(std::forward<input_t>(input), call_options));
  return unwrap_graph_invoke_status(
      wait_sender_result<wh::core::result<wh::compose::graph_invoke_result>>(std::move(sender)));
}

template <typename invokable_t, typename input_t>
  requires(!sender_request_invokable_with_movable_options<invokable_t, input_t>)
[[nodiscard]] auto invoke_value_sync(const invokable_t &invokable, input_t &&input,
                                     wh::core::run_context &context,
                                     wh::compose::graph_call_options &&)
    -> wh::core::result<wh::compose::graph_value> {
  return invoke_value_sync(invokable, std::forward<input_t>(input), context);
}

using synchronized_pair_gate_registry = wh::testing::helper::async_pair_gate_registry;

struct synchronized_async_dispatchers {
  using scheduler_type = synchronized_scheduler_helper::scheduler_type;

  explicit synchronized_async_dispatchers(const std::uint32_t thread_count)
      : threads_(std::max<std::uint32_t>(thread_count, 2U)),
        left_(threads_.scheduler_on_thread(0U)), right_(threads_.scheduler_on_thread(1U)) {}

  auto warm() -> void {
    auto left_ready =
        stdexec::sync_wait(stdexec::schedule(left_) | stdexec::then([] { return 0; }));
    REQUIRE(left_ready.has_value());

    auto right_ready =
        stdexec::sync_wait(stdexec::schedule(right_) | stdexec::then([] { return 0; }));
    REQUIRE(right_ready.has_value());
  }

  [[nodiscard]] auto left() const -> scheduler_type { return left_; }
  [[nodiscard]] auto right() const -> scheduler_type { return right_; }

private:
  synchronized_scheduler_helper threads_;
  scheduler_type left_;
  scheduler_type right_;
};

enum class synchronized_async_fan_in_shape : std::uint8_t {
  value = 0U,
  stream = 1U,
};

[[nodiscard]] auto
synchronized_dispatch_name(const wh::compose::graph_dispatch_policy dispatch_policy)
    -> std::string_view {
  switch (dispatch_policy) {
  case wh::compose::graph_dispatch_policy::same_wave:
    return "same-wave";
  case wh::compose::graph_dispatch_policy::next_wave:
    return "next-wave";
  }
  return "unknown";
}

[[nodiscard]] auto
make_synchronized_async_value_source(synchronized_async_dispatchers::scheduler_type scheduler,
                                     const std::shared_ptr<synchronized_pair_gate_registry> &gates,
                                     const int bias) {
  return [scheduler = std::move(scheduler), gates, bias](const wh::compose::graph_value &input,
                                                         wh::core::run_context &,
                                                         const wh::compose::graph_call_scope &) {
    auto copied_input = input;
    return wh::compose::detail::bridge_graph_sender(stdexec::starts_on(
        scheduler,
        stdexec::just(std::move(copied_input)) |
            stdexec::let_value(
                [gates, bias](wh::compose::graph_value payload) -> wh::compose::graph_sender {
                  auto typed = read_any<int>(std::move(payload));
                  if (typed.has_error()) {
                    return wh::compose::detail::failure_graph_sender(typed.error());
                  }
                  const int value = typed.value();
                  return wh::compose::detail::bridge_graph_sender(
                      gates->arrive(value) |
                      stdexec::then([value, bias]() -> wh::core::result<wh::compose::graph_value> {
                        return wh::core::any(value + bias);
                      }));
                })));
  };
}

[[nodiscard]] auto
make_synchronized_async_stream_source(synchronized_async_dispatchers::scheduler_type scheduler,
                                      const std::shared_ptr<synchronized_pair_gate_registry> &gates,
                                      const int bias) {
  return [scheduler = std::move(scheduler), gates,
          bias](const wh::compose::graph_value &input, wh::core::run_context &,
                const wh::compose::graph_call_scope &) -> graph_stream_result_sender {
    auto copied_input = input;
    auto sender = stdexec::starts_on(
        scheduler,
        stdexec::just(std::move(copied_input)) |
            stdexec::let_value([gates, bias](
                                   wh::compose::graph_value payload) -> graph_stream_result_sender {
              auto typed = read_any<int>(std::move(payload));
              if (typed.has_error()) {
                return graph_stream_result_sender{wh::core::detail::normalize_result_sender<
                    wh::core::result<wh::compose::graph_stream_reader>>(stdexec::just(
                    wh::core::result<wh::compose::graph_stream_reader>::failure(typed.error())))};
              }
              const int value = typed.value();
              return graph_stream_result_sender{wh::core::detail::normalize_result_sender<
                  wh::core::result<wh::compose::graph_stream_reader>>(
                  gates->arrive(value) |
                  stdexec::then(
                      [value, bias]() -> wh::core::result<wh::compose::graph_stream_reader> {
                        return make_int_graph_stream({value + bias, value + bias + 1});
                      }))};
            }));
    return graph_stream_result_sender{wh::core::detail::normalize_result_sender<
        wh::core::result<wh::compose::graph_stream_reader>>(std::move(sender))};
  };
}

[[nodiscard]] auto build_synchronized_async_fan_in_graph(
    const synchronized_async_fan_in_shape shape,
    const wh::compose::graph_dispatch_policy dispatch_policy,
    const std::shared_ptr<synchronized_async_dispatchers> &dispatchers,
    const std::shared_ptr<synchronized_pair_gate_registry> &gates)
    -> wh::core::result<wh::compose::graph> {
  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::dag;
  options.dispatch_policy = dispatch_policy;
  options.max_parallel_nodes = 2U;
  options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
  options.fan_in_policy = wh::compose::graph_fan_in_policy::require_all_sources;
  wh::compose::graph graph{std::move(options)};

  if (shape == synchronized_async_fan_in_shape::value) {
    REQUIRE(
        graph
            .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::value,
                        wh::compose::node_exec_mode::async>(
                "left_source", make_synchronized_async_value_source(dispatchers->left(), gates, 0))
            .has_value());
    REQUIRE(graph
                .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::value,
                            wh::compose::node_exec_mode::async>(
                    "right_source",
                    make_synchronized_async_value_source(dispatchers->right(), gates, 10))
                .has_value());
    REQUIRE(graph
                .add_lambda(
                    "merged_value",
                    [](const wh::compose::graph_value &input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_value> {
                      auto merged = read_any<wh::compose::graph_value_map>(input);
                      if (merged.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(merged.error());
                      }
                      int total = 0;
                      for (const auto &entry : merged.value()) {
                        auto typed = read_any<int>(entry.second);
                        if (typed.has_error()) {
                          return wh::core::result<wh::compose::graph_value>::failure(typed.error());
                        }
                        total += typed.value();
                      }
                      return wh::core::any(total);
                    })
                .has_value());
  } else {
    REQUIRE(
        graph
            .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream,
                        wh::compose::node_exec_mode::async>(
                "left_source", make_synchronized_async_stream_source(dispatchers->left(), gates, 0))
            .has_value());
    REQUIRE(graph
                .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream,
                            wh::compose::node_exec_mode::async>(
                    "right_source",
                    make_synchronized_async_stream_source(dispatchers->right(), gates, 10))
                .has_value());
    REQUIRE(graph
                .add_lambda("merged_value",
                            [](const wh::compose::graph_value &input, wh::core::run_context &,
                               const wh::compose::graph_call_scope &)
                                -> wh::core::result<wh::compose::graph_value> {
                              auto merged = read_any<wh::compose::graph_value_map>(input);
                              if (merged.has_error()) {
                                return wh::core::result<wh::compose::graph_value>::failure(
                                    merged.error());
                              }
                              int total = 0;
                              for (const auto &entry : merged.value()) {
                                auto chunks = sum_collected_graph_chunks(entry.second);
                                if (chunks.has_error()) {
                                  return wh::core::result<wh::compose::graph_value>::failure(
                                      chunks.error());
                                }
                                total += chunks.value();
                              }
                              return wh::core::any(total);
                            })
                .has_value());
  }

  REQUIRE(graph.add_entry_edge("left_source").has_value());
  REQUIRE(graph.add_entry_edge("right_source").has_value());
  REQUIRE(graph.add_edge("left_source", "merged_value").has_value());
  REQUIRE(graph.add_edge("right_source", "merged_value").has_value());
  REQUIRE(graph.add_exit_edge("merged_value").has_value());

  auto compiled = graph.compile();
  if (compiled.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(compiled.error());
  }
  return graph;
}

[[nodiscard]] auto
build_async_stream_collect_graph(const std::shared_ptr<exec::static_thread_pool> &pool)
    -> wh::core::result<wh::compose::graph> {
  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::dag;
  options.max_parallel_nodes = 1U;
  wh::compose::graph graph{std::move(options)};

  auto source = [pool](const wh::compose::graph_value &input, wh::core::run_context &,
                       const wh::compose::graph_call_scope &) {
    auto copied_input = input;
    return stdexec::starts_on(
        pool->get_scheduler(),
        stdexec::just(std::move(copied_input)) |
            stdexec::then([](wh::compose::graph_value payload)
                              -> wh::core::result<wh::compose::graph_stream_reader> {
              auto typed = read_any<int>(std::move(payload));
              if (typed.has_error()) {
                return wh::core::result<wh::compose::graph_stream_reader>::failure(typed.error());
              }
              return make_int_graph_stream({typed.value() + 3, typed.value() + 4});
            }));
  };

  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream,
                          wh::compose::node_exec_mode::async>("source", std::move(source))
              .has_value());
  REQUIRE(
      graph
          .add_component(
              wh::compose::make_component_node<
                  wh::core::component_kind::custom, wh::compose::node_contract::value,
                  wh::compose::node_contract::value, std::vector<wh::compose::graph_value>, int>(
                  "consumer", collected_sum_consumer{}))
          .has_value());
  REQUIRE(graph.add_entry_edge("source").has_value());
  REQUIRE(graph.add_edge("source", "consumer").has_value());
  REQUIRE(graph.add_exit_edge("consumer").has_value());

  auto compiled = graph.compile();
  if (compiled.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(compiled.error());
  }
  return graph;
}

[[nodiscard]] auto synchronized_expected_output(const synchronized_async_fan_in_shape shape,
                                                const int input) -> int {
  if (shape == synchronized_async_fan_in_shape::value) {
    return 2 * input + 10;
  }
  return 4 * input + 22;
}

[[nodiscard]] auto run_synchronized_async_fan_in_wave(const wh::compose::graph &graph,
                                                      const synchronized_async_fan_in_shape shape,
                                                      const int worker_count,
                                                      const int iterations_per_worker,
                                                      const int input_bias = 0)
    -> std::vector<std::string> {
  std::atomic<bool> failed{false};
  std::mutex error_mutex{};
  std::vector<std::string> errors{};
  std::vector<std::thread> workers{};
  workers.reserve(static_cast<std::size_t>(worker_count));

  for (int worker_id = 0; worker_id < worker_count; ++worker_id) {
    workers.emplace_back([&, worker_id]() {
      try {
        for (int iteration = 0; iteration < iterations_per_worker; ++iteration) {
          if (failed.load(std::memory_order_acquire)) {
            return;
          }

          const int input = input_bias + worker_id * 1000 + iteration;
          wh::core::run_context context{};
          auto invoked = invoke_value_sync(graph, wh::core::any(input), context);
          if (invoked.has_error()) {
            std::lock_guard lock{error_mutex};
            failed.store(true, std::memory_order_release);
            errors.push_back("invoke error:" + std::to_string(input));
            return;
          }

          auto typed = read_any<int>(std::move(invoked).value());
          if (typed.has_error()) {
            std::lock_guard lock{error_mutex};
            failed.store(true, std::memory_order_release);
            errors.push_back("type error:" + std::to_string(input));
            return;
          }

          const int expected = synchronized_expected_output(shape, input);
          if (typed.value() != expected) {
            std::lock_guard lock{error_mutex};
            failed.store(true, std::memory_order_release);
            errors.push_back("mismatch:" + std::to_string(input) + ":" +
                             std::to_string(typed.value()) + ":" + std::to_string(expected));
            return;
          }
        }
      } catch (const std::exception &error) {
        std::lock_guard lock{error_mutex};
        failed.store(true, std::memory_order_release);
        errors.push_back("exception:" + std::string{error.what()});
      } catch (...) {
        std::lock_guard lock{error_mutex};
        failed.store(true, std::memory_order_release);
        errors.push_back("exception:unknown");
      }
    });
  }

  for (auto &worker : workers) {
    worker.join();
  }

  if (!failed.load(std::memory_order_acquire)) {
    return {};
  }
  return errors;
}

template <typename fn_t> struct sync_embedding_impl {
  fn_t fn;

  [[nodiscard]] auto embed(const wh::embedding::embedding_request &request) const
      -> decltype(std::invoke(fn, request)) {
    return std::invoke(fn, request);
  }
};

template <typename fn_t> sync_embedding_impl(fn_t) -> sync_embedding_impl<fn_t>;

template <typename fn_t> struct sync_retriever_impl {
  fn_t fn;

  [[nodiscard]] auto retrieve(const wh::retriever::retriever_request &request) const
      -> decltype(std::invoke(fn, request)) {
    return std::invoke(fn, request);
  }
};

template <typename fn_t> sync_retriever_impl(fn_t) -> sync_retriever_impl<fn_t>;

template <typename fn_t> struct sync_indexer_batch_impl {
  fn_t fn;

  [[nodiscard]] auto write(const wh::indexer::indexer_request &request) const
      -> decltype(std::invoke(fn, request)) {
    return std::invoke(fn, request);
  }
};

template <typename fn_t> sync_indexer_batch_impl(fn_t) -> sync_indexer_batch_impl<fn_t>;

template <typename fn_t> struct sync_tool_stream_impl {
  fn_t fn;

  [[nodiscard]] auto stream(const wh::tool::tool_request &request) const
      -> decltype(std::invoke(fn, std::declval<std::string_view>(),
                              std::declval<const wh::tool::tool_options &>())) {
    return std::invoke(fn, std::string_view{request.input_json}, request.options);
  }
};

template <typename fn_t> sync_tool_stream_impl(fn_t) -> sync_tool_stream_impl<fn_t>;

template <typename fn_t> struct sync_tool_invoke_impl {
  fn_t fn;

  [[nodiscard]] auto invoke(const wh::tool::tool_request &request) const
      -> decltype(std::invoke(fn, std::declval<std::string_view>(),
                              std::declval<const wh::tool::tool_options &>())) {
    return std::invoke(fn, std::string_view{request.input_json}, request.options);
  }
};

template <typename fn_t> sync_tool_invoke_impl(fn_t) -> sync_tool_invoke_impl<fn_t>;

[[nodiscard]] inline auto add_test_node(wh::compose::graph &graph, wh::compose::component_node node)
    -> wh::core::result<void> {
  return graph.add_component(std::move(node));
}

[[nodiscard]] inline auto add_test_node(wh::compose::graph &graph, wh::compose::lambda_node node)
    -> wh::core::result<void> {
  return graph.add_lambda(std::move(node));
}

[[nodiscard]] inline auto add_test_node(wh::compose::graph &graph, wh::compose::subgraph_node node)
    -> wh::core::result<void> {
  return graph.add_subgraph(std::move(node));
}

[[nodiscard]] inline auto add_test_node(wh::compose::graph &graph, wh::compose::tools_node node)
    -> wh::core::result<void> {
  return graph.add_tools(std::move(node));
}

[[nodiscard]] inline auto add_test_node(wh::compose::graph &graph,
                                        wh::compose::passthrough_node node)
    -> wh::core::result<void> {
  return graph.add_passthrough(std::move(node));
}

struct compiled_graph_node_view {
  const wh::compose::compiled_node *node{nullptr};
};

struct test_nested_graph_state {
  [[nodiscard]] static auto scheduler() noexcept
      -> const wh::core::detail::any_resume_scheduler_t & {
    static const auto bound_scheduler =
        wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{});
    return bound_scheduler;
  }

  [[nodiscard]] static auto
  invoke(const void *, const wh::compose::graph &graph, wh::core::run_context &context,
         wh::compose::graph_value &input, const wh::compose::graph_call_scope *call_options,
         const wh::compose::node_path *path_prefix,
         wh::compose::graph_process_state *parent_process_state,
         wh::compose::detail::runtime_state::invoke_outputs *nested_outputs,
         const wh::compose::graph_node_trace *) -> wh::compose::graph_sender {
    return wh::compose::detail::start_scoped_graph(graph, context, input, call_options, path_prefix,
                                                   parent_process_state, nested_outputs,
                                                   scheduler(), scheduler());
  }
};

auto ensure_test_nested_runtime(wh::compose::node_runtime &runtime,
                                std::shared_ptr<test_nested_graph_state> &nested_state) -> void {
  if (!wh::compose::detail::node_runtime_access::nested_entry(runtime).bound()) {
    nested_state = std::make_shared<test_nested_graph_state>();
    wh::compose::detail::node_runtime_access::bind_internal(
        runtime, wh::compose::detail::node_runtime_access::invoke_outputs(runtime),
        wh::compose::nested_graph_entry{
            .state = nested_state.get(),
            .start = &test_nested_graph_state::invoke,
        });
  }
  if (runtime.control_scheduler() == nullptr) {
    runtime.set_control_scheduler(std::addressof(test_nested_graph_state::scheduler()));
  }
}

[[nodiscard]] auto compile_test_graph_node(const wh::compose::graph &graph,
                                           const std::string_view key = "worker")
    -> wh::core::result<compiled_graph_node_view> {
  auto compiled_node = graph.compiled_node_by_key(key);
  if (compiled_node.has_error()) {
    return wh::core::result<compiled_graph_node_view>::failure(compiled_node.error());
  }
  return compiled_graph_node_view{
      .node = std::addressof(compiled_node.value().get()),
  };
}

[[nodiscard]] auto run_compiled_test_graph_node(const compiled_graph_node_view view,
                                                wh::compose::graph_value input,
                                                wh::core::run_context &context,
                                                const wh::compose::node_runtime &runtime = {})
    -> wh::compose::graph_sender {
  if (view.node == nullptr) {
    return wh::compose::detail::failure_graph_sender(wh::core::errc::contract_violation);
  }
  auto owned_input = std::make_shared<wh::compose::graph_value>(std::move(input));
  auto base_runtime = runtime;
  auto nested_state = std::shared_ptr<test_nested_graph_state>{};
  ensure_test_nested_runtime(base_runtime, nested_state);
  auto owned_runtime = std::make_shared<wh::compose::node_runtime>(std::move(base_runtime));
  return wh::compose::graph_sender{wh::compose::detail::normalize_graph_sender(
      stdexec::just(std::move(owned_input), std::move(owned_runtime), std::move(nested_state)) |
      stdexec::let_value(
          [view, &context](std::shared_ptr<wh::compose::graph_value> owned_node_input,
                           std::shared_ptr<wh::compose::node_runtime> owned_runtime_state,
                           std::shared_ptr<test_nested_graph_state> owned_nested_state) mutable {
            if (wh::compose::compiled_node_is_sync(*view.node)) {
              return wh::compose::detail::bridge_graph_sender(
                  stdexec::just(wh::compose::run_compiled_sync_node(
                      *view.node, *owned_node_input, context, *owned_runtime_state)) |
                  stdexec::then([owned_node_input = std::move(owned_node_input),
                                 owned_runtime_state = std::move(owned_runtime_state),
                                 owned_nested_state = std::move(owned_nested_state)](
                                    wh::core::result<wh::compose::graph_value> status) mutable {
                    return status;
                  }));
            }
            return wh::compose::detail::bridge_graph_sender(
                wh::compose::run_compiled_async_node(*view.node, *owned_node_input, context,
                                                     *owned_runtime_state) |
                stdexec::then([owned_node_input = std::move(owned_node_input),
                               owned_runtime_state = std::move(owned_runtime_state),
                               owned_nested_state = std::move(owned_nested_state)](
                                  wh::core::result<wh::compose::graph_value> status) mutable {
                  return status;
                }));
          }))};
}

template <typename node_t>
[[nodiscard]] auto make_single_node_graph(node_t node,
                                          wh::compose::graph_compile_options options = {})
    -> wh::core::result<wh::compose::graph> {
  auto key = std::string{node.key()};
  auto boundary = wh::compose::graph_boundary{
      .input = node.input_contract(),
      .output = node.output_contract(),
  };
  wh::compose::graph graph{boundary, std::move(options)};
  auto added = add_test_node(graph, std::move(node));
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }
  auto start = graph.add_entry_edge(key);
  if (start.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(start.error());
  }
  auto finish = graph.add_exit_edge(key);
  if (finish.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(finish.error());
  }
  auto compiled = graph.compile();
  if (compiled.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(compiled.error());
  }
  return graph;
}

[[nodiscard]] constexpr auto runtime_modes() noexcept
    -> std::array<wh::compose::graph_runtime_mode, 2> {
  return {wh::compose::graph_runtime_mode::dag, wh::compose::graph_runtime_mode::pregel};
}

[[nodiscard]] constexpr auto runtime_mode_name(const wh::compose::graph_runtime_mode mode) noexcept
    -> std::string_view {
  switch (mode) {
  case wh::compose::graph_runtime_mode::dag:
    return "dag";
  case wh::compose::graph_runtime_mode::pregel:
    return "pregel";
  }
  return "unknown";
}

[[nodiscard]] auto checkpoint_pending_input(const wh::compose::checkpoint_state &state,
                                            const std::string_view key) -> wh::core::result<int> {
  const auto *payload = [&]() -> const wh::compose::graph_value * {
    const auto *pending = [&]() -> const wh::compose::checkpoint_pending_inputs * {
      if (state.runtime.dag.has_value() && !state.runtime.pregel.has_value()) {
        return std::addressof(state.runtime.dag->pending_inputs);
      }
      if (state.runtime.pregel.has_value() && !state.runtime.dag.has_value()) {
        return std::addressof(state.runtime.pregel->pending_inputs);
      }
      if (state.restore_shape.options.mode == wh::compose::graph_runtime_mode::pregel &&
          state.runtime.pregel.has_value()) {
        return std::addressof(state.runtime.pregel->pending_inputs);
      }
      if (state.runtime.dag.has_value()) {
        return std::addressof(state.runtime.dag->pending_inputs);
      }
      if (state.runtime.pregel.has_value()) {
        return std::addressof(state.runtime.pregel->pending_inputs);
      }
      return nullptr;
    }();
    if (pending == nullptr) {
      return nullptr;
    }
    if (key == wh::compose::graph_start_node_key) {
      return pending->entry.has_value() ? std::addressof(*pending->entry) : nullptr;
    }
    for (const auto &node_input : pending->nodes) {
      if (node_input.key == key) {
        return std::addressof(node_input.input);
      }
    }
    return nullptr;
  }();
  if (payload == nullptr) {
    return wh::core::result<int>::failure(wh::core::errc::not_found);
  }
  return read_any<int>(*payload);
}

[[nodiscard]] auto make_int_graph_stream(std::initializer_list<int> values)
    -> wh::core::result<wh::compose::graph_stream_reader> {
  std::vector<wh::compose::graph_value> chunks{};
  chunks.reserve(values.size());
  for (const auto value : values) {
    chunks.emplace_back(wh::core::any(value));
  }
  return wh::compose::make_values_stream_reader(std::move(chunks));
}

[[nodiscard]] auto make_string_graph_stream(std::initializer_list<std::string_view> values)
    -> wh::core::result<wh::compose::graph_stream_reader> {
  std::vector<wh::compose::graph_value> chunks{};
  chunks.reserve(values.size());
  for (const auto value : values) {
    chunks.emplace_back(wh::core::any(std::string{value}));
  }
  return wh::compose::make_values_stream_reader(std::move(chunks));
}

[[nodiscard]] auto collect_int_graph_stream(wh::compose::graph_stream_reader reader)
    -> wh::core::result<std::vector<int>> {
  auto chunks = wh::compose::collect_graph_stream_reader(std::move(reader));
  if (chunks.has_error()) {
    return wh::core::result<std::vector<int>>::failure(chunks.error());
  }

  std::vector<int> values{};
  values.reserve(chunks.value().size());
  for (auto &chunk : chunks.value()) {
    auto typed = read_any<int>(std::move(chunk));
    if (typed.has_error()) {
      return wh::core::result<std::vector<int>>::failure(typed.error());
    }
    values.push_back(typed.value());
  }
  return values;
}

[[nodiscard]] auto
make_collect_graph_sender(wh::compose::graph_stream_reader reader,
                          const wh::core::detail::any_resume_scheduler_t &graph_scheduler)
    -> wh::compose::graph_sender {
  return wh::compose::detail::bridge_graph_sender(wh::compose::detail::make_child_pump_sender(
      wh::compose::detail::collect_policy{
          .reader = std::move(reader),
      },
      graph_scheduler));
}

[[nodiscard]] inline auto make_auto_contract_edge_options() -> wh::compose::edge_options {
  return {};
}

[[nodiscard]] auto sum_collected_graph_chunks(const wh::compose::graph_value &input)
    -> wh::core::result<int> {
  auto collected = read_any<std::vector<wh::compose::graph_value>>(input);
  if (collected.has_error()) {
    return wh::core::result<int>::failure(collected.error());
  }

  int sum = 0;
  for (auto &entry : collected.value()) {
    auto typed = read_any<int>(std::move(entry));
    if (typed.has_error()) {
      return wh::core::result<int>::failure(typed.error());
    }
    sum += typed.value();
  }
  return sum;
}

template <typename sender_factory_t, typename validate_fn_t>
[[nodiscard]] auto
run_graph_sender_stress_wave(sender_factory_t make_sender, validate_fn_t validate_output,
                             const int worker_count, const int iterations_per_worker)
    -> std::vector<std::string> {
  std::atomic<bool> failed{false};
  std::mutex error_mutex{};
  std::vector<std::string> errors{};
  std::vector<std::thread> workers{};
  workers.reserve(static_cast<std::size_t>(worker_count));

  for (int worker_id = 0; worker_id < worker_count; ++worker_id) {
    workers.emplace_back([&, worker_id]() {
      try {
        for (int iteration = 0; iteration < iterations_per_worker; ++iteration) {
          const int base = worker_id * 1000 + iteration * 10;
          auto waited =
              wait_sender_result<wh::core::result<wh::compose::graph_value>>(make_sender(base));
          if (waited.has_error()) {
            std::lock_guard lock{error_mutex};
            failed.store(true, std::memory_order_release);
            errors.push_back("invoke error:" + std::to_string(base) + ":" +
                             waited.error().message());
            return;
          }

          if (auto validation = validate_output(base, std::move(waited).value());
              validation.has_value()) {
            std::lock_guard lock{error_mutex};
            failed.store(true, std::memory_order_release);
            errors.push_back(std::move(*validation));
            return;
          }
        }
      } catch (const std::exception &error) {
        std::lock_guard lock{error_mutex};
        failed.store(true, std::memory_order_release);
        errors.push_back("exception:" + std::string{error.what()});
      } catch (...) {
        std::lock_guard lock{error_mutex};
        failed.store(true, std::memory_order_release);
        errors.push_back("exception:unknown");
      }
    });
  }

  for (auto &worker : workers) {
    worker.join();
  }

  if (!failed.load(std::memory_order_acquire)) {
    return {};
  }
  return errors;
}

struct deep_nested_increment_graphs {
  wh::compose::graph leaf{};
  wh::compose::graph middle{};
  wh::compose::graph parent{};
  std::string leaf_name{};
  std::string middle_name{};
  std::string middle_path_key{};
  std::string leaf_path_key{};
};

[[nodiscard]] auto make_deep_nested_increment_graphs(const wh::compose::graph_runtime_mode mode)
    -> wh::core::result<deep_nested_increment_graphs> {
  wh::compose::graph_compile_options leaf_options{};
  leaf_options.mode = mode;
  leaf_options.name = "leaf_graph";
  wh::compose::graph leaf{std::move(leaf_options)};
  auto added_leaf = leaf.add_lambda(
      "inc",
      [](const wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
        auto typed = read_any<int>(input);
        if (typed.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(typed.error());
        }
        return wh::core::any(typed.value() + 1);
      });
  if (added_leaf.has_error()) {
    return wh::core::result<deep_nested_increment_graphs>::failure(added_leaf.error());
  }
  auto leaf_start = leaf.add_entry_edge("inc");
  if (leaf_start.has_error()) {
    return wh::core::result<deep_nested_increment_graphs>::failure(leaf_start.error());
  }
  auto leaf_end = leaf.add_exit_edge("inc");
  if (leaf_end.has_error()) {
    return wh::core::result<deep_nested_increment_graphs>::failure(leaf_end.error());
  }
  auto compiled_leaf = leaf.compile();
  if (compiled_leaf.has_error()) {
    return wh::core::result<deep_nested_increment_graphs>::failure(compiled_leaf.error());
  }

  wh::compose::graph_compile_options middle_options{};
  middle_options.mode = mode;
  middle_options.name = "middle_graph";
  wh::compose::graph middle{std::move(middle_options)};
  auto added_middle = middle.add_subgraph(wh::compose::make_subgraph_node("leaf", leaf));
  if (added_middle.has_error()) {
    return wh::core::result<deep_nested_increment_graphs>::failure(added_middle.error());
  }
  auto middle_start = middle.add_entry_edge("leaf");
  if (middle_start.has_error()) {
    return wh::core::result<deep_nested_increment_graphs>::failure(middle_start.error());
  }
  auto middle_end = middle.add_exit_edge("leaf");
  if (middle_end.has_error()) {
    return wh::core::result<deep_nested_increment_graphs>::failure(middle_end.error());
  }
  auto compiled_middle = middle.compile();
  if (compiled_middle.has_error()) {
    return wh::core::result<deep_nested_increment_graphs>::failure(compiled_middle.error());
  }

  wh::compose::graph_compile_options parent_options{};
  parent_options.mode = mode;
  parent_options.name = "parent_graph";
  wh::compose::graph parent{std::move(parent_options)};
  auto added_parent = parent.add_subgraph(wh::compose::make_subgraph_node("middle", middle));
  if (added_parent.has_error()) {
    return wh::core::result<deep_nested_increment_graphs>::failure(added_parent.error());
  }
  auto parent_start = parent.add_entry_edge("middle");
  if (parent_start.has_error()) {
    return wh::core::result<deep_nested_increment_graphs>::failure(parent_start.error());
  }
  auto parent_end = parent.add_exit_edge("middle");
  if (parent_end.has_error()) {
    return wh::core::result<deep_nested_increment_graphs>::failure(parent_end.error());
  }
  auto compiled_parent = parent.compile();
  if (compiled_parent.has_error()) {
    return wh::core::result<deep_nested_increment_graphs>::failure(compiled_parent.error());
  }

  return deep_nested_increment_graphs{
      .leaf = std::move(leaf),
      .middle = std::move(middle),
      .parent = std::move(parent),
      .leaf_name = "leaf_graph",
      .middle_name = "middle_graph",
      .middle_path_key = "middle",
      .leaf_path_key = "middle/leaf",
  };
}

[[nodiscard]] auto collect_tool_results(const wh::compose::graph_value &value)
    -> wh::core::result<std::reference_wrapper<const std::vector<wh::compose::tool_result>>> {
  if (const auto *results = wh::core::any_cast<std::vector<wh::compose::tool_result>>(&value);
      results != nullptr) {
    return std::cref(*results);
  }
  return wh::core::result<std::reference_wrapper<const std::vector<wh::compose::tool_result>>>::
      failure(wh::core::errc::type_mismatch);
}

[[nodiscard]] auto collect_tool_events(wh::compose::graph_stream_reader reader)
    -> wh::core::result<std::vector<wh::compose::tool_event>> {
  auto chunks = wh::compose::collect_graph_stream_reader(std::move(reader));
  if (chunks.has_error()) {
    return wh::core::result<std::vector<wh::compose::tool_event>>::failure(chunks.error());
  }

  std::vector<wh::compose::tool_event> events{};
  events.reserve(chunks.value().size());
  for (auto &chunk : chunks.value()) {
    auto event = read_any<wh::compose::tool_event>(std::move(chunk));
    if (event.has_error()) {
      return wh::core::result<std::vector<wh::compose::tool_event>>::failure(event.error());
    }
    events.push_back(std::move(event).value());
  }
  return events;
}

[[nodiscard]] constexpr auto sum_ints(std::span<const int> values) noexcept -> int {
  int sum = 0;
  for (const auto value : values) {
    sum += value;
  }
  return sum;
}

[[nodiscard]] auto sum_graph_value(const wh::compose::graph_value &value) -> wh::core::result<int> {
  if (const auto *typed = wh::core::any_cast<int>(&value); typed != nullptr) {
    return *typed;
  }
  if (const auto *chunks = wh::core::any_cast<std::vector<wh::compose::graph_value>>(&value);
      chunks != nullptr) {
    int total = 0;
    for (const auto &chunk : *chunks) {
      auto chunk_sum = sum_graph_value(chunk);
      if (chunk_sum.has_error()) {
        return wh::core::result<int>::failure(chunk_sum.error());
      }
      total += chunk_sum.value();
    }
    return total;
  }
  if (const auto *map = wh::core::any_cast<wh::compose::graph_value_map>(&value); map != nullptr) {
    int total = 0;
    for (const auto &[_, entry] : *map) {
      auto entry_sum = sum_graph_value(entry);
      if (entry_sum.has_error()) {
        return wh::core::result<int>::failure(entry_sum.error());
      }
      total += entry_sum.value();
    }
    return total;
  }
  return wh::core::result<int>::failure(wh::core::errc::type_mismatch);
}

[[nodiscard]] auto read_named_int(const wh::compose::graph_value_map &map,
                                  const std::string_view key) -> wh::core::result<int> {
  const auto iter = map.find(key);
  if (iter == map.end()) {
    return wh::core::result<int>::failure(wh::core::errc::not_found);
  }
  return read_any<int>(iter->second);
}

[[nodiscard]] auto make_chat_request(std::string text) -> wh::model::chat_request {
  wh::model::chat_request request{};
  wh::schema::message message{};
  message.role = wh::schema::message_role::user;
  message.parts.emplace_back(wh::schema::text_part{std::move(text)});
  request.messages.push_back(std::move(message));
  return request;
}

[[nodiscard]] auto make_prompt_request(std::string name) -> wh::prompt::prompt_render_request {
  wh::prompt::template_context placeholders{};
  placeholders.emplace("name", wh::prompt::template_value{std::move(name)});
  return wh::prompt::prompt_render_request{std::move(placeholders), {}};
}

[[nodiscard]] auto make_document_request(std::string content) -> wh::document::document_request {
  wh::document::document_request request{};
  request.source_kind = wh::document::document_source_kind::content;
  request.source = std::move(content);
  request.options.set_base(wh::document::loader_common_options{
      .parser = wh::document::parser_options{.uri = "mem://stress.txt"},
  });
  return request;
}

[[nodiscard]] auto make_tool_batch(std::initializer_list<wh::compose::tool_call> calls)
    -> wh::compose::tool_batch {
  return wh::compose::tool_batch{
      .calls = std::vector<wh::compose::tool_call>{calls},
  };
}

enum class node_case : std::uint8_t {
  lambda_vv_sync = 0U,
  lambda_vs_sync,
  lambda_sv_sync,
  lambda_ss_sync,
  lambda_vv_async,
  lambda_vs_async,
  lambda_sv_async,
  lambda_ss_async,
  component_custom_vv_sync,
  component_custom_vs_sync,
  component_custom_sv_sync,
  component_custom_ss_sync,
  component_custom_vv_async,
  component_custom_vs_async,
  component_custom_sv_async,
  component_custom_ss_async,
  component_embedding_vv,
  component_embedding_vv_async,
  component_model_vv,
  component_model_vs,
  component_model_vv_async,
  component_model_vs_async,
  component_prompt_vv,
  component_prompt_vv_async,
  component_retriever_vv,
  component_retriever_vv_async,
  component_indexer_vv,
  component_indexer_vv_async,
  component_document_vv,
  component_document_vv_async,
  component_tool_vv,
  component_tool_vs,
  component_tool_vv_async,
  component_tool_vs_async,
  passthrough_value,
  passthrough_stream,
  tools_vv_sync,
  tools_vs_sync,
  tools_vv_async,
  tools_vs_async,
  subgraph_vv,
  subgraph_vs,
  subgraph_sv,
  subgraph_ss,
};

struct node_case_info {
  std::string_view name{};
  wh::compose::node_contract from{wh::compose::node_contract::value};
  wh::compose::node_contract to{wh::compose::node_contract::value};
};

[[nodiscard]] constexpr auto describe_node_case(const node_case value) noexcept -> node_case_info {
  using contract = wh::compose::node_contract;

  switch (value) {
  case node_case::lambda_vv_sync:
    return {"lambda_vv_sync", contract::value, contract::value};
  case node_case::lambda_vs_sync:
    return {"lambda_vs_sync", contract::value, contract::stream};
  case node_case::lambda_sv_sync:
    return {"lambda_sv_sync", contract::stream, contract::value};
  case node_case::lambda_ss_sync:
    return {"lambda_ss_sync", contract::stream, contract::stream};
  case node_case::lambda_vv_async:
    return {"lambda_vv_async", contract::value, contract::value};
  case node_case::lambda_vs_async:
    return {"lambda_vs_async", contract::value, contract::stream};
  case node_case::lambda_sv_async:
    return {"lambda_sv_async", contract::stream, contract::value};
  case node_case::lambda_ss_async:
    return {"lambda_ss_async", contract::stream, contract::stream};
  case node_case::component_custom_vv_sync:
    return {"component_custom_vv_sync", contract::value, contract::value};
  case node_case::component_custom_vs_sync:
    return {"component_custom_vs_sync", contract::value, contract::stream};
  case node_case::component_custom_sv_sync:
    return {"component_custom_sv_sync", contract::stream, contract::value};
  case node_case::component_custom_ss_sync:
    return {"component_custom_ss_sync", contract::stream, contract::stream};
  case node_case::component_custom_vv_async:
    return {"component_custom_vv_async", contract::value, contract::value};
  case node_case::component_custom_vs_async:
    return {"component_custom_vs_async", contract::value, contract::stream};
  case node_case::component_custom_sv_async:
    return {"component_custom_sv_async", contract::stream, contract::value};
  case node_case::component_custom_ss_async:
    return {"component_custom_ss_async", contract::stream, contract::stream};
  case node_case::component_embedding_vv:
    return {"component_embedding_vv", contract::value, contract::value};
  case node_case::component_embedding_vv_async:
    return {"component_embedding_vv_async", contract::value, contract::value};
  case node_case::component_model_vv:
    return {"component_model_vv", contract::value, contract::value};
  case node_case::component_model_vs:
    return {"component_model_vs", contract::value, contract::stream};
  case node_case::component_model_vv_async:
    return {"component_model_vv_async", contract::value, contract::value};
  case node_case::component_model_vs_async:
    return {"component_model_vs_async", contract::value, contract::stream};
  case node_case::component_prompt_vv:
    return {"component_prompt_vv", contract::value, contract::value};
  case node_case::component_prompt_vv_async:
    return {"component_prompt_vv_async", contract::value, contract::value};
  case node_case::component_retriever_vv:
    return {"component_retriever_vv", contract::value, contract::value};
  case node_case::component_retriever_vv_async:
    return {"component_retriever_vv_async", contract::value, contract::value};
  case node_case::component_indexer_vv:
    return {"component_indexer_vv", contract::value, contract::value};
  case node_case::component_indexer_vv_async:
    return {"component_indexer_vv_async", contract::value, contract::value};
  case node_case::component_document_vv:
    return {"component_document_vv", contract::value, contract::value};
  case node_case::component_document_vv_async:
    return {"component_document_vv_async", contract::value, contract::value};
  case node_case::component_tool_vv:
    return {"component_tool_vv", contract::value, contract::value};
  case node_case::component_tool_vs:
    return {"component_tool_vs", contract::value, contract::stream};
  case node_case::component_tool_vv_async:
    return {"component_tool_vv_async", contract::value, contract::value};
  case node_case::component_tool_vs_async:
    return {"component_tool_vs_async", contract::value, contract::stream};
  case node_case::passthrough_value:
    return {"passthrough_value", contract::value, contract::value};
  case node_case::passthrough_stream:
    return {"passthrough_stream", contract::stream, contract::stream};
  case node_case::tools_vv_sync:
    return {"tools_vv_sync", contract::value, contract::value};
  case node_case::tools_vs_sync:
    return {"tools_vs_sync", contract::value, contract::stream};
  case node_case::tools_vv_async:
    return {"tools_vv_async", contract::value, contract::value};
  case node_case::tools_vs_async:
    return {"tools_vs_async", contract::value, contract::stream};
  case node_case::subgraph_vv:
    return {"subgraph_vv", contract::value, contract::value};
  case node_case::subgraph_vs:
    return {"subgraph_vs", contract::value, contract::stream};
  case node_case::subgraph_sv:
    return {"subgraph_sv", contract::stream, contract::value};
  case node_case::subgraph_ss:
    return {"subgraph_ss", contract::stream, contract::stream};
  }

  return {};
}

[[nodiscard]] constexpr auto all_node_cases() noexcept -> std::array<node_case, 44> {
  return {node_case::lambda_vv_sync,
          node_case::lambda_vs_sync,
          node_case::lambda_sv_sync,
          node_case::lambda_ss_sync,
          node_case::lambda_vv_async,
          node_case::lambda_vs_async,
          node_case::lambda_sv_async,
          node_case::lambda_ss_async,
          node_case::component_custom_vv_sync,
          node_case::component_custom_vs_sync,
          node_case::component_custom_sv_sync,
          node_case::component_custom_ss_sync,
          node_case::component_custom_vv_async,
          node_case::component_custom_vs_async,
          node_case::component_custom_sv_async,
          node_case::component_custom_ss_async,
          node_case::component_embedding_vv,
          node_case::component_embedding_vv_async,
          node_case::component_model_vv,
          node_case::component_model_vs,
          node_case::component_model_vv_async,
          node_case::component_model_vs_async,
          node_case::component_prompt_vv,
          node_case::component_prompt_vv_async,
          node_case::component_retriever_vv,
          node_case::component_retriever_vv_async,
          node_case::component_indexer_vv,
          node_case::component_indexer_vv_async,
          node_case::component_document_vv,
          node_case::component_document_vv_async,
          node_case::component_tool_vv,
          node_case::component_tool_vs,
          node_case::component_tool_vv_async,
          node_case::component_tool_vs_async,
          node_case::passthrough_value,
          node_case::passthrough_stream,
          node_case::tools_vv_sync,
          node_case::tools_vs_sync,
          node_case::tools_vv_async,
          node_case::tools_vs_async,
          node_case::subgraph_vv,
          node_case::subgraph_vs,
          node_case::subgraph_sv,
          node_case::subgraph_ss};
}

[[nodiscard]] constexpr auto node_case_name(const node_case value) noexcept -> std::string_view {
  return describe_node_case(value).name;
}

[[nodiscard]] auto make_subgraph_case(const node_case value)
    -> wh::core::result<wh::compose::graph> {
  auto child_boundary = wh::compose::graph_boundary{};
  switch (value) {
  case node_case::subgraph_vs:
    child_boundary.output = wh::compose::node_contract::stream;
    break;
  case node_case::subgraph_sv:
    child_boundary.input = wh::compose::node_contract::stream;
    break;
  case node_case::subgraph_ss:
    child_boundary.input = wh::compose::node_contract::stream;
    child_boundary.output = wh::compose::node_contract::stream;
    break;
  default:
    break;
  }
  wh::compose::graph child{child_boundary};
  switch (value) {
  case node_case::subgraph_vv: {
    auto added = child.add_lambda(
        "leaf",
        [](const wh::compose::graph_value &input, wh::core::run_context &,
           const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
          auto typed = read_any<int>(input);
          if (typed.has_error()) {
            return wh::core::result<wh::compose::graph_value>::failure(typed.error());
          }
          return wh::core::any(typed.value() + 4);
        });
    if (added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(added.error());
    }
    break;
  }
  case node_case::subgraph_vs: {
    auto added =
        child.add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream>(
            "leaf",
            [](const wh::compose::graph_value &input, wh::core::run_context &,
               const wh::compose::graph_call_scope &)
                -> wh::core::result<wh::compose::graph_stream_reader> {
              auto typed = read_any<int>(input);
              if (typed.has_error()) {
                return wh::core::result<wh::compose::graph_stream_reader>::failure(typed.error());
              }
              return make_int_graph_stream({typed.value() + 4, typed.value() + 5});
            });
    if (added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(added.error());
    }
    break;
  }
  case node_case::subgraph_sv: {
    auto added =
        child.add_lambda<wh::compose::node_contract::stream, wh::compose::node_contract::value>(
            "leaf",
            [](wh::compose::graph_stream_reader input, wh::core::run_context &,
               const wh::compose::graph_call_scope &)
                -> wh::core::result<wh::compose::graph_value> {
              auto values = collect_int_graph_stream(std::move(input));
              if (values.has_error()) {
                return wh::core::result<wh::compose::graph_value>::failure(values.error());
              }
              return wh::core::any(sum_ints(values.value()) + 6);
            });
    if (added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(added.error());
    }
    break;
  }
  case node_case::subgraph_ss: {
    auto added =
        child.add_lambda<wh::compose::node_contract::stream, wh::compose::node_contract::stream>(
            "leaf",
            [](wh::compose::graph_stream_reader input, wh::core::run_context &,
               const wh::compose::graph_call_scope &)
                -> wh::core::result<wh::compose::graph_stream_reader> {
              auto values = collect_int_graph_stream(std::move(input));
              if (values.has_error()) {
                return wh::core::result<wh::compose::graph_stream_reader>::failure(values.error());
              }
              std::vector<wh::compose::graph_value> output{};
              output.reserve(values.value().size());
              for (const auto item : values.value()) {
                output.emplace_back(wh::core::any(item + 7));
              }
              return wh::compose::make_values_stream_reader(std::move(output));
            });
    if (added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(added.error());
    }
    break;
  }
  default:
    return wh::core::result<wh::compose::graph>::failure(wh::core::errc::invalid_argument);
  }

  auto start = child.add_entry_edge("leaf");
  if (start.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(start.error());
  }
  auto finish = child.add_exit_edge("leaf");
  if (finish.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(finish.error());
  }
  auto compiled = child.compile();
  if (compiled.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(compiled.error());
  }
  return child;
}

[[nodiscard]] auto build_node_graph(const node_case value,
                                    const wh::compose::graph_runtime_mode mode)
    -> wh::core::result<wh::compose::graph> {
  wh::compose::graph_compile_options options{};
  options.mode = mode;
  const auto graph_options = [&options] {
    auto copy = options;
    return copy;
  };

  switch (value) {
  case node_case::lambda_vv_sync:
    return make_single_node_graph(
        wh::compose::make_lambda_node(
            "worker",
            [](const wh::compose::graph_value &input, wh::core::run_context &,
               const wh::compose::graph_call_scope &)
                -> wh::core::result<wh::compose::graph_value> {
              auto typed = read_any<int>(input);
              if (typed.has_error()) {
                return wh::core::result<wh::compose::graph_value>::failure(typed.error());
              }
              return wh::core::any(typed.value() + 1);
            }),
        std::move(options));
  case node_case::lambda_vs_sync:
    return make_single_node_graph(
        wh::compose::make_lambda_node<wh::compose::node_contract::value,
                                      wh::compose::node_contract::stream>(
            "worker",
            [](const wh::compose::graph_value &input, wh::core::run_context &,
               const wh::compose::graph_call_scope &)
                -> wh::core::result<wh::compose::graph_stream_reader> {
              auto typed = read_any<int>(input);
              if (typed.has_error()) {
                return wh::core::result<wh::compose::graph_stream_reader>::failure(typed.error());
              }
              return make_int_graph_stream({typed.value() + 1, typed.value() + 2});
            }),
        std::move(options));
  case node_case::lambda_sv_sync:
    return make_single_node_graph(
        wh::compose::make_lambda_node<wh::compose::node_contract::stream,
                                      wh::compose::node_contract::value>(
            "worker",
            [](wh::compose::graph_stream_reader input, wh::core::run_context &,
               const wh::compose::graph_call_scope &)
                -> wh::core::result<wh::compose::graph_value> {
              auto values = collect_int_graph_stream(std::move(input));
              if (values.has_error()) {
                return wh::core::result<wh::compose::graph_value>::failure(values.error());
              }
              return wh::core::any(sum_ints(values.value()));
            }),
        std::move(options));
  case node_case::lambda_ss_sync:
    return make_single_node_graph(
        wh::compose::make_lambda_node<wh::compose::node_contract::stream,
                                      wh::compose::node_contract::stream>(
            "worker",
            [](wh::compose::graph_stream_reader input, wh::core::run_context &,
               const wh::compose::graph_call_scope &)
                -> wh::core::result<wh::compose::graph_stream_reader> {
              auto values = collect_int_graph_stream(std::move(input));
              if (values.has_error()) {
                return wh::core::result<wh::compose::graph_stream_reader>::failure(values.error());
              }
              std::vector<wh::compose::graph_value> output{};
              output.reserve(values.value().size());
              for (const auto item : values.value()) {
                output.emplace_back(wh::core::any(item * 2));
              }
              return wh::compose::make_values_stream_reader(std::move(output));
            }),
        std::move(options));
  case node_case::lambda_vv_async:
    return make_single_node_graph(
        wh::compose::make_lambda_node<wh::compose::node_contract::value,
                                      wh::compose::node_contract::value,
                                      wh::compose::node_exec_mode::async>(
            "worker",
            [](const wh::compose::graph_value &input, wh::core::run_context &,
               const wh::compose::graph_call_scope &) {
              auto typed = read_any<int>(input);
              if (typed.has_error()) {
                return stdexec::just(
                    wh::core::result<wh::compose::graph_value>::failure(typed.error()));
              }
              return stdexec::just(
                  wh::core::result<wh::compose::graph_value>{wh::core::any(typed.value() + 20)});
            }),
        std::move(options));
  case node_case::lambda_vs_async:
    return make_single_node_graph(
        wh::compose::make_lambda_node<wh::compose::node_contract::value,
                                      wh::compose::node_contract::stream,
                                      wh::compose::node_exec_mode::async>(
            "worker",
            [](const wh::compose::graph_value &input, wh::core::run_context &,
               const wh::compose::graph_call_scope &) {
              auto typed = read_any<int>(input);
              if (typed.has_error()) {
                return stdexec::just(
                    wh::core::result<wh::compose::graph_stream_reader>::failure(typed.error()));
              }
              return stdexec::just(wh::core::result<wh::compose::graph_stream_reader>{
                  make_int_graph_stream({typed.value() + 21, typed.value() + 22}).value()});
            }),
        std::move(options));
  case node_case::lambda_sv_async:
    return make_single_node_graph(
        wh::compose::make_lambda_node<wh::compose::node_contract::stream,
                                      wh::compose::node_contract::value,
                                      wh::compose::node_exec_mode::async>(
            "worker",
            [](wh::compose::graph_stream_reader input, wh::core::run_context &,
               const wh::compose::graph_call_scope &) {
              auto values = collect_int_graph_stream(std::move(input));
              if (values.has_error()) {
                return stdexec::just(
                    wh::core::result<wh::compose::graph_value>::failure(values.error()));
              }
              return stdexec::just(wh::core::result<wh::compose::graph_value>{
                  wh::core::any(sum_ints(values.value()) + 40)});
            }),
        std::move(options));
  case node_case::lambda_ss_async:
    return make_single_node_graph(
        wh::compose::make_lambda_node<wh::compose::node_contract::stream,
                                      wh::compose::node_contract::stream,
                                      wh::compose::node_exec_mode::async>(
            "worker",
            [](wh::compose::graph_stream_reader input, wh::core::run_context &,
               const wh::compose::graph_call_scope &) {
              auto values = collect_int_graph_stream(std::move(input));
              if (values.has_error()) {
                return stdexec::just(
                    wh::core::result<wh::compose::graph_stream_reader>::failure(values.error()));
              }
              std::vector<wh::compose::graph_value> output{};
              output.reserve(values.value().size());
              for (const auto item : values.value()) {
                output.emplace_back(wh::core::any(item + 50));
              }
              return stdexec::just(wh::core::result<wh::compose::graph_stream_reader>{
                  wh::compose::make_values_stream_reader(std::move(output)).value()});
            }),
        std::move(options));
  case node_case::component_custom_vv_sync: {
    struct component {
      [[nodiscard]] auto invoke(const int &value, wh::core::run_context &) const
          -> wh::core::result<int> {
        return value + 5;
      }
    };
    return make_single_node_graph(
        wh::compose::make_component_node<wh::compose::component_kind::custom,
                                         wh::compose::node_contract::value,
                                         wh::compose::node_contract::value, int, int>("worker",
                                                                                      component{}),
        graph_options());
  }
  case node_case::component_custom_vs_sync: {
    struct component {
      [[nodiscard]] auto stream(const int &value, wh::core::run_context &) const
          -> wh::core::result<wh::compose::graph_stream_reader> {
        return make_int_graph_stream({value + 10});
      }
    };
    return make_single_node_graph(
        wh::compose::make_component_node<
            wh::compose::component_kind::custom, wh::compose::node_contract::value,
            wh::compose::node_contract::stream, int, wh::compose::graph_stream_reader>("worker",
                                                                                       component{}),
        graph_options());
  }
  case node_case::component_custom_sv_sync: {
    struct component {
      [[nodiscard]] auto invoke(wh::compose::graph_stream_reader reader,
                                wh::core::run_context &) const -> wh::core::result<int> {
        auto values = collect_int_graph_stream(std::move(reader));
        if (values.has_error()) {
          return wh::core::result<int>::failure(values.error());
        }
        return sum_ints(values.value());
      }
    };
    return make_single_node_graph(
        wh::compose::make_component_node<
            wh::compose::component_kind::custom, wh::compose::node_contract::stream,
            wh::compose::node_contract::value, wh::compose::graph_stream_reader, int>("worker",
                                                                                      component{}),
        graph_options());
  }
  case node_case::component_custom_ss_sync: {
    struct component {
      [[nodiscard]] auto stream(wh::compose::graph_stream_reader reader,
                                wh::core::run_context &) const
          -> wh::core::result<wh::compose::graph_stream_reader> {
        auto values = collect_int_graph_stream(std::move(reader));
        if (values.has_error()) {
          return wh::core::result<wh::compose::graph_stream_reader>::failure(values.error());
        }
        std::vector<wh::compose::graph_value> output{};
        output.reserve(values.value().size());
        for (const auto item : values.value()) {
          output.emplace_back(wh::core::any(item * 2));
        }
        return wh::compose::make_values_stream_reader(std::move(output));
      }
    };
    return make_single_node_graph(
        wh::compose::make_component_node<
            wh::compose::component_kind::custom, wh::compose::node_contract::stream,
            wh::compose::node_contract::stream, wh::compose::graph_stream_reader,
            wh::compose::graph_stream_reader>("worker", component{}),
        graph_options());
  }
  case node_case::component_custom_vv_async: {
    struct counts {
      int async_calls{0};
    };
    struct component {
      std::shared_ptr<counts> state;

      [[nodiscard]] auto async_invoke(const int &value, wh::core::run_context &) const {
        ++state->async_calls;
        return stdexec::just(wh::core::result<int>{value + 30});
      }
    };
    return make_single_node_graph(
        wh::compose::make_component_node<
            wh::compose::component_kind::custom, wh::compose::node_contract::value,
            wh::compose::node_contract::value, int, int, wh::compose::node_exec_mode::async>(
            "worker", component{std::make_shared<counts>()}),
        graph_options());
  }
  case node_case::component_custom_vs_async: {
    struct component {
      [[nodiscard]] auto async_stream(const int &value, wh::core::run_context &) const {
        return stdexec::just(wh::core::result<wh::compose::graph_stream_reader>{
            make_int_graph_stream({value + 31, value + 32}).value()});
      }
    };
    return make_single_node_graph(
        wh::compose::make_component_node<
            wh::compose::component_kind::custom, wh::compose::node_contract::value,
            wh::compose::node_contract::stream, int, wh::compose::graph_stream_reader,
            wh::compose::node_exec_mode::async>("worker", component{}),
        graph_options());
  }
  case node_case::component_custom_sv_async: {
    struct component {
      [[nodiscard]] auto async_invoke(wh::compose::graph_stream_reader reader,
                                      wh::core::run_context &) const {
        auto values = collect_int_graph_stream(std::move(reader));
        if (values.has_error()) {
          return stdexec::just(wh::core::result<int>::failure(values.error()));
        }
        return stdexec::just(wh::core::result<int>{sum_ints(values.value()) + 33});
      }
    };
    return make_single_node_graph(
        wh::compose::make_component_node<
            wh::compose::component_kind::custom, wh::compose::node_contract::stream,
            wh::compose::node_contract::value, wh::compose::graph_stream_reader, int,
            wh::compose::node_exec_mode::async>("worker", component{}),
        graph_options());
  }
  case node_case::component_custom_ss_async: {
    struct component {
      [[nodiscard]] auto async_stream(wh::compose::graph_stream_reader reader,
                                      wh::core::run_context &) const {
        auto values = collect_int_graph_stream(std::move(reader));
        if (values.has_error()) {
          return stdexec::just(
              wh::core::result<wh::compose::graph_stream_reader>::failure(values.error()));
        }
        std::vector<wh::compose::graph_value> output{};
        output.reserve(values.value().size());
        for (const auto item : values.value()) {
          output.emplace_back(wh::core::any(item + 34));
        }
        return stdexec::just(wh::core::result<wh::compose::graph_stream_reader>{
            wh::compose::make_values_stream_reader(std::move(output)).value()});
      }
    };
    return make_single_node_graph(
        wh::compose::make_component_node<
            wh::compose::component_kind::custom, wh::compose::node_contract::stream,
            wh::compose::node_contract::stream, wh::compose::graph_stream_reader,
            wh::compose::graph_stream_reader, wh::compose::node_exec_mode::async>("worker",
                                                                                  component{}),
        graph_options());
  }
  case node_case::component_embedding_vv: {
    auto node = wh::compose::make_component_node<wh::compose::component_kind::embedding,
                                                 wh::compose::node_contract::value,
                                                 wh::compose::node_contract::value>(
        "worker", wh::embedding::embedding{sync_embedding_impl{
                      [](const wh::embedding::embedding_request &request)
                          -> wh::core::result<wh::embedding::embedding_response> {
                        wh::embedding::embedding_response output{};
                        output.reserve(request.inputs.size());
                        for (const auto &entry : request.inputs) {
                          output.push_back(std::vector<double>{static_cast<double>(entry.size())});
                        }
                        return output;
                      }}});
    return make_single_node_graph(std::move(node), graph_options());
  }
  case node_case::component_embedding_vv_async: {
    struct impl {
      [[nodiscard]] auto embed_sender(wh::embedding::embedding_request &&request) const {
        return stdexec::just(
            wh::core::result<wh::embedding::embedding_response>{wh::embedding::embedding_response{
                std::vector<double>{static_cast<double>(request.inputs.size() + 10U)}}});
      }
    };
    auto node = wh::compose::make_component_node<
        wh::compose::component_kind::embedding, wh::compose::node_contract::value,
        wh::compose::node_contract::value, wh::compose::node_exec_mode::async>(
        "worker", wh::embedding::embedding{impl{}});
    return make_single_node_graph(std::move(node), graph_options());
  }
  case node_case::component_model_vv:
    return make_single_node_graph(
        wh::compose::make_component_node<wh::compose::component_kind::model,
                                         wh::compose::node_contract::value,
                                         wh::compose::node_contract::value>(
            "worker", wh::model::echo_chat_model{}),
        graph_options());
  case node_case::component_model_vs:
    return make_single_node_graph(
        wh::compose::make_component_node<wh::compose::component_kind::model,
                                         wh::compose::node_contract::value,
                                         wh::compose::node_contract::stream>(
            "worker", wh::model::echo_chat_model{}),
        graph_options());
  case node_case::component_model_vv_async: {
    struct impl {
      [[nodiscard]] auto invoke(const wh::model::chat_request &request) const
          -> wh::core::result<wh::model::chat_response> {
        wh::model::chat_response response{};
        response.message = request.messages.front();
        return response;
      }

      [[nodiscard]] auto stream(const wh::model::chat_request &request) const
          -> wh::core::result<wh::model::chat_message_stream_reader> {
        auto [writer, reader] = wh::schema::stream::make_pipe_stream<wh::schema::message>(1U);
        auto message = request.messages.front();
        auto wrote = writer.try_write(std::move(message));
        if (wrote.has_error()) {
          return wh::core::result<wh::model::chat_message_stream_reader>::failure(wrote.error());
        }
        auto closed = writer.close();
        if (closed.has_error()) {
          return wh::core::result<wh::model::chat_message_stream_reader>::failure(closed.error());
        }
        return wh::model::chat_message_stream_reader{std::move(reader)};
      }

      [[nodiscard]] auto invoke_sender(wh::model::chat_request request) const {
        return stdexec::just(invoke(request));
      }

      [[nodiscard]] auto stream_sender(wh::model::chat_request request) const {
        return stdexec::just(stream(request));
      }
    };
    return make_single_node_graph(
        wh::compose::make_component_node<
            wh::compose::component_kind::model, wh::compose::node_contract::value,
            wh::compose::node_contract::value, wh::compose::node_exec_mode::async>(
            "worker", wh::model::chat_model{impl{}}),
        graph_options());
  }
  case node_case::component_model_vs_async: {
    struct impl {
      [[nodiscard]] auto invoke(const wh::model::chat_request &request) const
          -> wh::core::result<wh::model::chat_response> {
        wh::model::chat_response response{};
        response.message = request.messages.front();
        return response;
      }

      [[nodiscard]] auto stream(const wh::model::chat_request &request) const
          -> wh::core::result<wh::model::chat_message_stream_reader> {
        auto [writer, reader] = wh::schema::stream::make_pipe_stream<wh::schema::message>(1U);
        auto message = request.messages.front();
        auto wrote = writer.try_write(std::move(message));
        if (wrote.has_error()) {
          return wh::core::result<wh::model::chat_message_stream_reader>::failure(wrote.error());
        }
        auto closed = writer.close();
        if (closed.has_error()) {
          return wh::core::result<wh::model::chat_message_stream_reader>::failure(closed.error());
        }
        return wh::model::chat_message_stream_reader{std::move(reader)};
      }

      [[nodiscard]] auto invoke_sender(wh::model::chat_request request) const {
        return stdexec::just(invoke(request));
      }

      [[nodiscard]] auto stream_sender(wh::model::chat_request request) const {
        return stdexec::just(stream(request));
      }
    };
    return make_single_node_graph(
        wh::compose::make_component_node<
            wh::compose::component_kind::model, wh::compose::node_contract::value,
            wh::compose::node_contract::stream, wh::compose::node_exec_mode::async>(
            "worker", wh::model::chat_model{impl{}}),
        graph_options());
  }
  case node_case::component_prompt_vv: {
    wh::prompt::simple_chat_template prompt({wh::prompt::prompt_message_template{
        wh::schema::message_role::user, "Hello {{name}}", "user"}});
    return make_single_node_graph(
        wh::compose::make_component_node<wh::compose::component_kind::prompt,
                                         wh::compose::node_contract::value,
                                         wh::compose::node_contract::value>("worker",
                                                                            std::move(prompt)),
        graph_options());
  }
  case node_case::component_prompt_vv_async: {
    struct impl {
      [[nodiscard]] auto render_sender(wh::prompt::prompt_render_request request) const {
        std::string name = "unknown";
        const auto iter = request.context.find("name");
        if (iter != request.context.end()) {
          if (const auto *text = iter->second.string_if(); text != nullptr) {
            name = *text;
          }
        }
        std::vector<wh::schema::message> messages{};
        wh::schema::message message{};
        message.role = wh::schema::message_role::user;
        message.parts.emplace_back(wh::schema::text_part{"Hello " + name});
        messages.push_back(std::move(message));
        return stdexec::just(
            wh::core::result<std::vector<wh::schema::message>>{std::move(messages)});
      }
    };
    return make_single_node_graph(
        wh::compose::make_component_node<
            wh::compose::component_kind::prompt, wh::compose::node_contract::value,
            wh::compose::node_contract::value, wh::compose::node_exec_mode::async>(
            "worker", wh::prompt::chat_template{impl{}}),
        graph_options());
  }
  case node_case::component_retriever_vv: {
    auto node = wh::compose::make_component_node<wh::compose::component_kind::retriever,
                                                 wh::compose::node_contract::value,
                                                 wh::compose::node_contract::value>(
        "worker", wh::retriever::retriever{sync_retriever_impl{
                      [](const wh::retriever::retriever_request &request)
                          -> wh::core::result<wh::retriever::retriever_response> {
                        return wh::retriever::retriever_response{
                            wh::schema::document{"hit:" + request.query}};
                      }}});
    return make_single_node_graph(std::move(node), graph_options());
  }
  case node_case::component_retriever_vv_async: {
    struct impl {
      [[nodiscard]] auto retrieve_sender(wh::retriever::retriever_request request) const {
        return stdexec::just(wh::core::result<wh::retriever::retriever_response>{
            wh::retriever::retriever_response{wh::schema::document{"hit:" + request.query}}});
      }
    };
    return make_single_node_graph(
        wh::compose::make_component_node<
            wh::compose::component_kind::retriever, wh::compose::node_contract::value,
            wh::compose::node_contract::value, wh::compose::node_exec_mode::async>(
            "worker", wh::retriever::retriever{impl{}}),
        graph_options());
  }
  case node_case::component_indexer_vv: {
    auto node = wh::compose::make_component_node<wh::compose::component_kind::indexer,
                                                 wh::compose::node_contract::value,
                                                 wh::compose::node_contract::value>(
        "worker", wh::indexer::indexer{sync_indexer_batch_impl{
                      [](const wh::indexer::indexer_request &request)
                          -> wh::core::result<wh::indexer::indexer_response> {
                        wh::indexer::indexer_response response{};
                        response.success_count = request.documents.size();
                        response.document_ids.resize(request.documents.size(), "indexed");
                        return response;
                      }}});
    return make_single_node_graph(std::move(node), graph_options());
  }
  case node_case::component_indexer_vv_async: {
    struct impl {
      [[nodiscard]] auto write_sender(wh::indexer::indexer_request request) const {
        wh::indexer::indexer_response response{};
        response.success_count = request.documents.size();
        response.document_ids.resize(request.documents.size(), "indexed-async");
        return stdexec::just(wh::core::result<wh::indexer::indexer_response>{std::move(response)});
      }
    };
    return make_single_node_graph(
        wh::compose::make_component_node<
            wh::compose::component_kind::indexer, wh::compose::node_contract::value,
            wh::compose::node_contract::value, wh::compose::node_exec_mode::async>(
            "worker", wh::indexer::indexer{impl{}}),
        graph_options());
  }
  case node_case::component_document_vv: {
    auto node = wh::compose::make_component_node<wh::compose::component_kind::document,
                                                 wh::compose::node_contract::value,
                                                 wh::compose::node_contract::value>(
        "worker", wh::document::document{
                      wh::document::document_processor{wh::document::parser::make_text_parser()}});
    return make_single_node_graph(std::move(node), graph_options());
  }
  case node_case::component_document_vv_async: {
    struct impl {
      [[nodiscard]] auto process_sender(wh::document::document_request request) const {
        wh::document::document_batch batch{};
        batch.push_back(wh::schema::document{request.source});
        return stdexec::just(wh::core::result<wh::document::document_batch>{std::move(batch)});
      }
    };
    return make_single_node_graph(
        wh::compose::make_component_node<
            wh::compose::component_kind::document, wh::compose::node_contract::value,
            wh::compose::node_contract::value, wh::compose::node_exec_mode::async>(
            "worker", wh::document::document{impl{}}),
        graph_options());
  }
  case node_case::component_tool_vv: {
    wh::schema::tool_schema_definition schema{};
    schema.name = "echo";
    auto node = wh::compose::make_component_node<wh::compose::component_kind::tool,
                                                 wh::compose::node_contract::value,
                                                 wh::compose::node_contract::value>(
        "worker",
        wh::tool::tool{std::move(schema),
                       sync_tool_invoke_impl{
                           [](const std::string_view input,
                              const wh::tool::tool_options &) -> wh::core::result<std::string> {
                             return std::string{"tool:"} + std::string{input};
                           }}});
    return make_single_node_graph(std::move(node), graph_options());
  }
  case node_case::component_tool_vs: {
    wh::schema::tool_schema_definition schema{};
    schema.name = "echo";
    auto node = wh::compose::make_component_node<wh::compose::component_kind::tool,
                                                 wh::compose::node_contract::value,
                                                 wh::compose::node_contract::stream>(
        "worker",
        wh::tool::tool{
            std::move(schema),
            sync_tool_stream_impl{[](const std::string_view input, const wh::tool::tool_options &)
                                      -> wh::core::result<wh::tool::tool_output_stream_reader> {
              return wh::tool::tool_output_stream_reader{
                  wh::schema::stream::make_values_stream_reader(
                      std::vector<std::string>{std::string{input}})};
            }}});
    return make_single_node_graph(std::move(node), graph_options());
  }
  case node_case::component_tool_vv_async: {
    struct impl {
      [[nodiscard]] auto invoke_sender(wh::tool::tool_request request) const {
        return stdexec::just(
            wh::core::result<std::string>{std::string{"tool:"} + request.input_json});
      }

      [[nodiscard]] auto stream_sender(wh::tool::tool_request request) const {
        return stdexec::just(wh::core::result<wh::tool::tool_output_stream_reader>{
            wh::tool::tool_output_stream_reader{wh::schema::stream::make_values_stream_reader(
                std::vector<std::string>{request.input_json})}});
      }
    };
    wh::schema::tool_schema_definition schema{};
    schema.name = "echo";
    auto node = wh::compose::make_component_node<
        wh::compose::component_kind::tool, wh::compose::node_contract::value,
        wh::compose::node_contract::value, wh::compose::node_exec_mode::async>(
        "worker", wh::tool::tool{std::move(schema), impl{}});
    return make_single_node_graph(std::move(node), graph_options());
  }
  case node_case::component_tool_vs_async: {
    struct impl {
      [[nodiscard]] auto invoke_sender(wh::tool::tool_request request) const {
        return stdexec::just(
            wh::core::result<std::string>{std::string{"tool:"} + request.input_json});
      }

      [[nodiscard]] auto stream_sender(wh::tool::tool_request request) const {
        return stdexec::just(wh::core::result<wh::tool::tool_output_stream_reader>{
            wh::tool::tool_output_stream_reader{wh::schema::stream::make_values_stream_reader(
                std::vector<std::string>{request.input_json + ":async"})}});
      }
    };
    wh::schema::tool_schema_definition schema{};
    schema.name = "echo";
    auto node = wh::compose::make_component_node<
        wh::compose::component_kind::tool, wh::compose::node_contract::value,
        wh::compose::node_contract::stream, wh::compose::node_exec_mode::async>(
        "worker", wh::tool::tool{std::move(schema), impl{}});
    return make_single_node_graph(std::move(node), graph_options());
  }
  case node_case::passthrough_value:
    return make_single_node_graph(
        wh::compose::make_passthrough_node<wh::compose::node_contract::value>("worker"),
        graph_options());
  case node_case::passthrough_stream:
    return make_single_node_graph(
        wh::compose::make_passthrough_node<wh::compose::node_contract::stream>("worker"),
        graph_options());
  case node_case::tools_vv_sync: {
    wh::compose::tool_registry registry{};
    registry.insert_or_assign(
        "echo",
        wh::compose::tool_entry{
            .invoke = [](const wh::compose::tool_call &call,
                         wh::tool::call_scope) -> wh::core::result<wh::compose::graph_value> {
              return wh::core::any(std::string{"sync:"} + call.arguments);
            }});
    return make_single_node_graph(wh::compose::make_tools_node("worker", std::move(registry)),
                                  graph_options());
  }
  case node_case::tools_vs_sync: {
    wh::compose::tool_registry registry{};
    registry.insert_or_assign(
        "echo", wh::compose::tool_entry{.stream = [](const wh::compose::tool_call &call,
                                                     wh::tool::call_scope)
                                            -> wh::core::result<wh::compose::graph_stream_reader> {
          return make_string_graph_stream({call.arguments + ":1", call.arguments + ":2"});
        }});
    return make_single_node_graph(wh::compose::make_tools_node<wh::compose::node_contract::value,
                                                               wh::compose::node_contract::stream>(
                                      "worker", std::move(registry)),
                                  graph_options());
  }
  case node_case::tools_vv_async: {
    wh::compose::tool_registry registry{};
    registry.insert_or_assign(
        "echo",
        wh::compose::tool_entry{.async_invoke =
                                    [](wh::compose::tool_call call,
                                       wh::tool::call_scope) -> wh::compose::tools_invoke_sender {
          return stdexec::just(std::move(call.arguments)) |
                 stdexec::then([](std::string text) -> wh::core::result<wh::compose::graph_value> {
                   return wh::core::any(std::string{"async:"} + text);
                 });
        }});
    return make_single_node_graph(wh::compose::make_tools_node<wh::compose::node_contract::value,
                                                               wh::compose::node_contract::value,
                                                               wh::compose::node_exec_mode::async>(
                                      "worker", std::move(registry)),
                                  graph_options());
  }
  case node_case::tools_vs_async: {
    wh::compose::tool_registry registry{};
    registry.insert_or_assign(
        "echo",
        wh::compose::tool_entry{.async_stream =
                                    [](wh::compose::tool_call call,
                                       wh::tool::call_scope) -> wh::compose::tools_stream_sender {
          return stdexec::just(std::move(call.arguments)) |
                 stdexec::then(
                     [](std::string text) -> wh::core::result<wh::compose::graph_stream_reader> {
                       return make_string_graph_stream({text + ":a", text + ":b"});
                     });
        }});
    return make_single_node_graph(wh::compose::make_tools_node<wh::compose::node_contract::value,
                                                               wh::compose::node_contract::stream,
                                                               wh::compose::node_exec_mode::async>(
                                      "worker", std::move(registry)),
                                  graph_options());
  }
  case node_case::subgraph_vv:
  case node_case::subgraph_vs:
  case node_case::subgraph_sv:
  case node_case::subgraph_ss: {
    auto child = make_subgraph_case(value);
    if (child.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(child.error());
    }
    switch (value) {
    case node_case::subgraph_vv:
      return make_single_node_graph(
          wh::compose::make_subgraph_node("worker", std::move(child).value()), graph_options());
    case node_case::subgraph_vs:
      return make_single_node_graph(
          wh::compose::make_subgraph_node("worker", std::move(child).value()), graph_options());
    case node_case::subgraph_sv:
      return make_single_node_graph(
          wh::compose::make_subgraph_node("worker", std::move(child).value()), graph_options());
    case node_case::subgraph_ss:
      return make_single_node_graph(
          wh::compose::make_subgraph_node("worker", std::move(child).value()), graph_options());
    default:
      break;
    }
    break;
  }
  }
  return wh::core::result<wh::compose::graph>::failure(wh::core::errc::invalid_argument);
}

[[nodiscard]] auto make_node_input(const node_case value, const int iteration)
    -> wh::core::result<wh::compose::graph_value> {
  switch (value) {
  case node_case::lambda_vv_sync:
  case node_case::lambda_vs_sync:
  case node_case::lambda_vv_async:
  case node_case::lambda_vs_async:
  case node_case::component_custom_vv_sync:
  case node_case::component_custom_vs_sync:
  case node_case::component_custom_vv_async:
  case node_case::component_custom_vs_async:
  case node_case::passthrough_value:
  case node_case::subgraph_vv:
  case node_case::subgraph_vs:
    return wh::core::any(iteration);
  case node_case::lambda_sv_sync:
  case node_case::lambda_ss_sync:
  case node_case::lambda_sv_async:
  case node_case::lambda_ss_async:
  case node_case::component_custom_sv_sync:
  case node_case::component_custom_ss_sync:
  case node_case::component_custom_sv_async:
  case node_case::component_custom_ss_async:
  case node_case::passthrough_stream:
  case node_case::subgraph_sv:
  case node_case::subgraph_ss: {
    auto reader = make_int_graph_stream({iteration, iteration + 1, iteration + 2});
    if (reader.has_error()) {
      return wh::core::result<wh::compose::graph_value>::failure(reader.error());
    }
    return wh::core::any(std::move(reader).value());
  }
  case node_case::component_embedding_vv:
    return wh::core::any(wh::embedding::embedding_request{
        .inputs = std::vector<std::string>{
            std::string{"a"}, std::string(static_cast<std::size_t>(iteration % 3 + 2), 'b')}});
  case node_case::component_embedding_vv_async:
    return wh::core::any(wh::embedding::embedding_request{.inputs = std::vector<std::string>{"x"}});
  case node_case::component_model_vv:
  case node_case::component_model_vs:
  case node_case::component_model_vv_async:
  case node_case::component_model_vs_async:
    return wh::core::any(make_chat_request("hello-" + std::to_string(iteration)));
  case node_case::component_prompt_vv:
  case node_case::component_prompt_vv_async:
    return wh::core::any(make_prompt_request("name-" + std::to_string(iteration)));
  case node_case::component_retriever_vv:
  case node_case::component_retriever_vv_async:
    return wh::core::any(wh::retriever::retriever_request{.query = "q-" + std::to_string(iteration),
                                                          .index = "idx"});
  case node_case::component_indexer_vv:
  case node_case::component_indexer_vv_async:
    return wh::core::any(wh::indexer::indexer_request{
        .documents = {wh::schema::document{"doc-" + std::to_string(iteration)},
                      wh::schema::document{"doc-extra"}}});
  case node_case::component_document_vv:
  case node_case::component_document_vv_async:
    return wh::core::any(make_document_request("content-" + std::to_string(iteration)));
  case node_case::component_tool_vv:
  case node_case::component_tool_vs:
  case node_case::component_tool_vv_async:
  case node_case::component_tool_vs_async:
    return wh::core::any(
        wh::tool::tool_request{.input_json = "payload-" + std::to_string(iteration)});
  case node_case::tools_vv_sync:
  case node_case::tools_vs_sync:
  case node_case::tools_vv_async:
  case node_case::tools_vs_async:
    return wh::core::any(make_tool_batch({wh::compose::tool_call{
        .call_id = "call-" + std::to_string(iteration),
        .tool_name = "echo",
        .arguments = "payload-" + std::to_string(iteration),
    }}));
  }
  return wh::core::result<wh::compose::graph_value>::failure(wh::core::errc::invalid_argument);
}

inline auto verify_node_output(const node_case value, wh::compose::graph_value output,
                               const int iteration) -> void {
  switch (value) {
  case node_case::lambda_vv_sync:
    REQUIRE(read_any<int>(output).value() == iteration + 1);
    break;
  case node_case::lambda_vs_sync: {
    auto reader = read_any<wh::compose::graph_stream_reader>(std::move(output));
    REQUIRE(reader.has_value());
    auto values = collect_int_graph_stream(std::move(reader).value());
    REQUIRE(values.has_value());
    REQUIRE(values.value() == std::vector<int>{iteration + 1, iteration + 2});
    break;
  }
  case node_case::lambda_sv_sync:
    REQUIRE(read_any<int>(output).value() == 3 * iteration + 3);
    break;
  case node_case::lambda_ss_sync: {
    auto reader = read_any<wh::compose::graph_stream_reader>(std::move(output));
    REQUIRE(reader.has_value());
    auto values = collect_int_graph_stream(std::move(reader).value());
    REQUIRE(values.has_value());
    REQUIRE(values.value() ==
            std::vector<int>{2 * iteration, 2 * (iteration + 1), 2 * (iteration + 2)});
    break;
  }
  case node_case::lambda_vv_async:
    REQUIRE(read_any<int>(output).value() == iteration + 20);
    break;
  case node_case::lambda_vs_async: {
    auto reader = read_any<wh::compose::graph_stream_reader>(std::move(output));
    REQUIRE(reader.has_value());
    auto values = collect_int_graph_stream(std::move(reader).value());
    REQUIRE(values.has_value());
    REQUIRE(values.value() == std::vector<int>{iteration + 21, iteration + 22});
    break;
  }
  case node_case::lambda_sv_async:
    REQUIRE(read_any<int>(output).value() == 3 * iteration + 43);
    break;
  case node_case::lambda_ss_async: {
    auto reader = read_any<wh::compose::graph_stream_reader>(std::move(output));
    REQUIRE(reader.has_value());
    auto values = collect_int_graph_stream(std::move(reader).value());
    REQUIRE(values.has_value());
    REQUIRE(values.value() == std::vector<int>{iteration + 50, iteration + 51, iteration + 52});
    break;
  }
  case node_case::component_custom_vv_sync:
    REQUIRE(read_any<int>(output).value() == iteration + 5);
    break;
  case node_case::component_custom_vs_sync: {
    auto reader = read_any<wh::compose::graph_stream_reader>(std::move(output));
    REQUIRE(reader.has_value());
    auto values = collect_int_graph_stream(std::move(reader).value());
    REQUIRE(values.has_value());
    REQUIRE(values.value() == std::vector<int>{iteration + 10});
    break;
  }
  case node_case::component_custom_sv_sync:
    REQUIRE(read_any<int>(output).value() == 3 * iteration + 3);
    break;
  case node_case::component_custom_ss_sync: {
    auto reader = read_any<wh::compose::graph_stream_reader>(std::move(output));
    REQUIRE(reader.has_value());
    auto values = collect_int_graph_stream(std::move(reader).value());
    REQUIRE(values.has_value());
    REQUIRE(values.value() ==
            std::vector<int>{2 * iteration, 2 * (iteration + 1), 2 * (iteration + 2)});
    break;
  }
  case node_case::component_custom_vv_async:
    REQUIRE(read_any<int>(output).value() == iteration + 30);
    break;
  case node_case::component_custom_vs_async: {
    auto reader = read_any<wh::compose::graph_stream_reader>(std::move(output));
    REQUIRE(reader.has_value());
    auto values = collect_int_graph_stream(std::move(reader).value());
    REQUIRE(values.has_value());
    REQUIRE(values.value() == std::vector<int>{iteration + 31, iteration + 32});
    break;
  }
  case node_case::component_custom_sv_async:
    REQUIRE(read_any<int>(output).value() == 3 * iteration + 36);
    break;
  case node_case::component_custom_ss_async: {
    auto reader = read_any<wh::compose::graph_stream_reader>(std::move(output));
    REQUIRE(reader.has_value());
    auto values = collect_int_graph_stream(std::move(reader).value());
    REQUIRE(values.has_value());
    REQUIRE(values.value() == std::vector<int>{iteration + 34, iteration + 35, iteration + 36});
    break;
  }
  case node_case::component_embedding_vv: {
    auto typed = read_any<wh::embedding::embedding_response>(output);
    REQUIRE(typed.has_value());
    REQUIRE(typed.value().size() == 2U);
    REQUIRE(typed.value()[0] == std::vector<double>{1.0});
    REQUIRE(typed.value()[1] == std::vector<double>{static_cast<double>(iteration % 3 + 2)});
    break;
  }
  case node_case::component_embedding_vv_async: {
    auto typed = read_any<wh::embedding::embedding_response>(output);
    REQUIRE(typed.has_value());
    REQUIRE(typed.value().size() == 1U);
    REQUIRE(typed.value()[0] == std::vector<double>{11.0});
    break;
  }
  case node_case::component_model_vv: {
    auto typed = read_any<wh::model::chat_response>(output);
    REQUIRE(typed.has_value());
    REQUIRE(std::get<wh::schema::text_part>(typed.value().message.parts.front()).text ==
            "hello-" + std::to_string(iteration));
    break;
  }
  case node_case::component_model_vs: {
    auto reader = read_any<wh::compose::graph_stream_reader>(std::move(output));
    REQUIRE(reader.has_value());
    auto next = reader.value().read();
    REQUIRE(next.has_value());
    REQUIRE(next.value().value.has_value());
    auto message = read_any<wh::schema::message>(std::move(*next.value().value));
    REQUIRE(message.has_value());
    REQUIRE(std::get<wh::schema::text_part>(message.value().parts.front()).text ==
            "hello-" + std::to_string(iteration));
    break;
  }
  case node_case::component_model_vv_async: {
    auto typed = read_any<wh::model::chat_response>(output);
    REQUIRE(typed.has_value());
    REQUIRE(std::get<wh::schema::text_part>(typed.value().message.parts.front()).text ==
            "hello-" + std::to_string(iteration));
    break;
  }
  case node_case::component_model_vs_async: {
    auto reader = read_any<wh::compose::graph_stream_reader>(std::move(output));
    REQUIRE(reader.has_value());
    auto next = reader.value().read();
    REQUIRE(next.has_value());
    REQUIRE(next.value().value.has_value());
    auto message = read_any<wh::schema::message>(std::move(*next.value().value));
    REQUIRE(message.has_value());
    REQUIRE(std::get<wh::schema::text_part>(message.value().parts.front()).text ==
            "hello-" + std::to_string(iteration));
    break;
  }
  case node_case::component_prompt_vv: {
    auto typed = read_any<std::vector<wh::schema::message>>(output);
    REQUIRE(typed.has_value());
    REQUIRE(typed.value().size() == 1U);
    REQUIRE(std::get<wh::schema::text_part>(typed.value().front().parts.front()).text ==
            "Hello name-" + std::to_string(iteration));
    break;
  }
  case node_case::component_prompt_vv_async: {
    auto typed = read_any<std::vector<wh::schema::message>>(output);
    REQUIRE(typed.has_value());
    REQUIRE(typed.value().size() == 1U);
    REQUIRE(std::get<wh::schema::text_part>(typed.value().front().parts.front()).text ==
            "Hello name-" + std::to_string(iteration));
    break;
  }
  case node_case::component_retriever_vv: {
    auto typed = read_any<wh::retriever::retriever_response>(output);
    REQUIRE(typed.has_value());
    REQUIRE(typed.value().size() == 1U);
    REQUIRE(typed.value().front().content() == "hit:q-" + std::to_string(iteration));
    break;
  }
  case node_case::component_retriever_vv_async: {
    auto typed = read_any<wh::retriever::retriever_response>(output);
    REQUIRE(typed.has_value());
    REQUIRE(typed.value().size() == 1U);
    REQUIRE(typed.value().front().content() == "hit:q-" + std::to_string(iteration));
    break;
  }
  case node_case::component_indexer_vv: {
    auto typed = read_any<wh::indexer::indexer_response>(output);
    REQUIRE(typed.has_value());
    REQUIRE(typed.value().success_count == 2U);
    REQUIRE(typed.value().document_ids == std::vector<std::string>{"indexed", "indexed"});
    break;
  }
  case node_case::component_indexer_vv_async: {
    auto typed = read_any<wh::indexer::indexer_response>(output);
    REQUIRE(typed.has_value());
    REQUIRE(typed.value().success_count == 2U);
    REQUIRE(typed.value().document_ids ==
            std::vector<std::string>{"indexed-async", "indexed-async"});
    break;
  }
  case node_case::component_document_vv: {
    auto typed = read_any<wh::document::document_batch>(output);
    REQUIRE(typed.has_value());
    REQUIRE(typed.value().size() == 1U);
    REQUIRE(typed.value().front().content() == "content-" + std::to_string(iteration));
    break;
  }
  case node_case::component_document_vv_async: {
    auto typed = read_any<wh::document::document_batch>(output);
    REQUIRE(typed.has_value());
    REQUIRE(typed.value().size() == 1U);
    REQUIRE(typed.value().front().content() == "content-" + std::to_string(iteration));
    break;
  }
  case node_case::component_tool_vv:
    REQUIRE(read_any<std::string>(output).value() == "tool:payload-" + std::to_string(iteration));
    break;
  case node_case::component_tool_vs: {
    auto reader = read_any<wh::compose::graph_stream_reader>(std::move(output));
    REQUIRE(reader.has_value());
    auto next = reader.value().read();
    REQUIRE(next.has_value());
    REQUIRE(next.value().value.has_value());
    auto text = read_any<std::string>(std::move(*next.value().value));
    REQUIRE(text.has_value());
    REQUIRE(text.value() == "payload-" + std::to_string(iteration));
    break;
  }
  case node_case::component_tool_vv_async:
    REQUIRE(read_any<std::string>(output).value() == "tool:payload-" + std::to_string(iteration));
    break;
  case node_case::component_tool_vs_async: {
    auto reader = read_any<wh::compose::graph_stream_reader>(std::move(output));
    REQUIRE(reader.has_value());
    auto next = reader.value().read();
    REQUIRE(next.has_value());
    REQUIRE(next.value().value.has_value());
    auto text = read_any<std::string>(std::move(*next.value().value));
    REQUIRE(text.has_value());
    REQUIRE(text.value() == "payload-" + std::to_string(iteration) + ":async");
    break;
  }
  case node_case::passthrough_value:
    REQUIRE(read_any<int>(output).value() == iteration);
    break;
  case node_case::passthrough_stream: {
    auto reader = read_any<wh::compose::graph_stream_reader>(std::move(output));
    REQUIRE(reader.has_value());
    auto values = collect_int_graph_stream(std::move(reader).value());
    REQUIRE(values.has_value());
    REQUIRE(values.value() == std::vector<int>{iteration, iteration + 1, iteration + 2});
    break;
  }
  case node_case::tools_vv_sync: {
    auto results = collect_tool_results(output);
    REQUIRE(results.has_value());
    REQUIRE(results.value().get().size() == 1U);
    REQUIRE(read_any<std::string>(results.value().get()[0].value).value() ==
            "sync:payload-" + std::to_string(iteration));
    break;
  }
  case node_case::tools_vs_sync: {
    auto reader = read_any<wh::compose::graph_stream_reader>(std::move(output));
    REQUIRE(reader.has_value());
    auto events = collect_tool_events(std::move(reader).value());
    REQUIRE(events.has_value());
    REQUIRE(events.value().size() == 2U);
    REQUIRE(read_any<std::string>(events.value()[0].value).value() ==
            "payload-" + std::to_string(iteration) + ":1");
    REQUIRE(read_any<std::string>(events.value()[1].value).value() ==
            "payload-" + std::to_string(iteration) + ":2");
    break;
  }
  case node_case::tools_vv_async: {
    auto results = collect_tool_results(output);
    REQUIRE(results.has_value());
    REQUIRE(results.value().get().size() == 1U);
    REQUIRE(read_any<std::string>(results.value().get()[0].value).value() ==
            "async:payload-" + std::to_string(iteration));
    break;
  }
  case node_case::tools_vs_async: {
    auto reader = read_any<wh::compose::graph_stream_reader>(std::move(output));
    REQUIRE(reader.has_value());
    auto events = collect_tool_events(std::move(reader).value());
    REQUIRE(events.has_value());
    REQUIRE(events.value().size() == 2U);
    REQUIRE(read_any<std::string>(events.value()[0].value).value() ==
            "payload-" + std::to_string(iteration) + ":a");
    REQUIRE(read_any<std::string>(events.value()[1].value).value() ==
            "payload-" + std::to_string(iteration) + ":b");
    break;
  }
  case node_case::subgraph_vv:
    REQUIRE(read_any<int>(output).value() == iteration + 4);
    break;
  case node_case::subgraph_vs: {
    auto reader = read_any<wh::compose::graph_stream_reader>(std::move(output));
    REQUIRE(reader.has_value());
    auto values = collect_int_graph_stream(std::move(reader).value());
    REQUIRE(values.has_value());
    REQUIRE(values.value() == std::vector<int>{iteration + 4, iteration + 5});
    break;
  }
  case node_case::subgraph_sv:
    REQUIRE(read_any<int>(output).value() == 3 * iteration + 9);
    break;
  case node_case::subgraph_ss: {
    auto reader = read_any<wh::compose::graph_stream_reader>(std::move(output));
    REQUIRE(reader.has_value());
    auto values = collect_int_graph_stream(std::move(reader).value());
    REQUIRE(values.has_value());
    REQUIRE(values.value() == std::vector<int>{iteration + 7, iteration + 8, iteration + 9});
    break;
  }
  }
}

enum class source_shape : std::uint8_t {
  single_value = 0U,
  single_stream,
  multi_value,
  multi_stream,
  mixed,
};

enum class target_shape : std::uint8_t {
  single_value = 0U,
  single_stream,
  multi_value,
  multi_stream,
};

enum class lowering_style : std::uint8_t {
  builtin = 0U,
  custom,
};

[[nodiscard]] constexpr auto all_source_shapes() noexcept -> std::array<source_shape, 5> {
  return {source_shape::single_value, source_shape::single_stream, source_shape::multi_value,
          source_shape::multi_stream, source_shape::mixed};
}

[[nodiscard]] constexpr auto all_target_shapes() noexcept -> std::array<target_shape, 4> {
  return {target_shape::single_value, target_shape::single_stream, target_shape::multi_value,
          target_shape::multi_stream};
}

[[nodiscard]] constexpr auto all_lowering_styles() noexcept -> std::array<lowering_style, 2> {
  return {lowering_style::builtin, lowering_style::custom};
}

[[nodiscard]] constexpr auto shape_name(const source_shape value) noexcept -> std::string_view {
  switch (value) {
  case source_shape::single_value:
    return "single_value";
  case source_shape::single_stream:
    return "single_stream";
  case source_shape::multi_value:
    return "multi_value";
  case source_shape::multi_stream:
    return "multi_stream";
  case source_shape::mixed:
    return "mixed";
  }
  return "unknown";
}

[[nodiscard]] constexpr auto shape_name(const target_shape value) noexcept -> std::string_view {
  switch (value) {
  case target_shape::single_value:
    return "single_value";
  case target_shape::single_stream:
    return "single_stream";
  case target_shape::multi_value:
    return "multi_value";
  case target_shape::multi_stream:
    return "multi_stream";
  }
  return "unknown";
}

[[nodiscard]] constexpr auto style_name(const lowering_style value) noexcept -> std::string_view {
  switch (value) {
  case lowering_style::builtin:
    return "builtin";
  case lowering_style::custom:
    return "custom";
  }
  return "unknown";
}

[[nodiscard]] auto make_builtin_bridge_options(const bool source_stream, const bool target_stream)
    -> std::optional<wh::compose::edge_options> {
  return source_stream == target_stream
             ? std::nullopt
             : std::optional<wh::compose::edge_options>{wh::compose::edge_options{}};
}

[[nodiscard]] constexpr auto base_sum(const source_shape shape, const int iteration) noexcept
    -> int {
  switch (shape) {
  case source_shape::single_value:
    return iteration + 1;
  case source_shape::single_stream:
    return 2 * iteration + 3;
  case source_shape::multi_value:
    return 2 * iteration + 11;
  case source_shape::multi_stream:
    return 4 * iteration + 24;
  case source_shape::mixed:
    return 3 * iteration + 22;
  }
  return 0;
}

[[nodiscard]] auto add_value_source(wh::compose::graph &graph, std::string_view key,
                                    const int offset) -> wh::core::result<void> {
  return graph.add_lambda(
      std::string{key},
      [offset](
          const wh::compose::graph_value &input, wh::core::run_context &,
          const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
        auto typed = read_any<int>(input);
        if (typed.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(typed.error());
        }
        return wh::core::any(typed.value() + offset);
      });
}

[[nodiscard]] auto add_stream_source(wh::compose::graph &graph, std::string_view key,
                                     const int offset) -> wh::core::result<void> {
  return graph.add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream>(
      std::string{key},
      [offset](const wh::compose::graph_value &input, wh::core::run_context &,
               const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_stream_reader> {
        auto typed = read_any<int>(input);
        if (typed.has_error()) {
          return wh::core::result<wh::compose::graph_stream_reader>::failure(typed.error());
        }
        return make_int_graph_stream({typed.value() + offset, typed.value() + offset + 1});
      });
}

[[nodiscard]] auto add_value_sink(wh::compose::graph &graph, std::string_view key, const int bias)
    -> wh::core::result<void> {
  return graph.add_lambda(
      std::string{key},
      [bias](const wh::compose::graph_value &input, wh::core::run_context &,
             const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
        auto total = sum_graph_value(input);
        if (total.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(total.error());
        }
        return wh::core::any(total.value() + bias);
      });
}

[[nodiscard]] auto add_stream_sink(wh::compose::graph &graph, std::string_view key, const int bias)
    -> wh::core::result<void> {
  return graph.add_lambda<wh::compose::node_contract::stream, wh::compose::node_contract::value>(
      std::string{key},
      [bias](wh::compose::graph_stream_reader input, wh::core::run_context &,
             const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
        auto values = collect_int_graph_stream(std::move(input));
        if (values.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(values.error());
        }
        return wh::core::any(sum_ints(values.value()) + bias);
      });
}

[[nodiscard]] auto make_custom_bridge_options(const bool source_stream, const bool target_stream)
    -> std::optional<wh::compose::edge_options> {
  if (source_stream == target_stream) {
    return std::nullopt;
  }

  wh::compose::edge_options options{};
  options.adapter.kind = wh::compose::edge_adapter_kind::custom;
  if (!source_stream && target_stream) {
    options.adapter.custom.to_stream = wh::compose::edge_to_stream_adapter{
        [](wh::compose::graph_value &&input, const wh::compose::edge_limits &,
           wh::core::run_context &) -> wh::core::result<wh::compose::graph_stream_reader> {
          return wh::compose::make_single_value_stream_reader(std::move(input));
        }};
    return options;
  }

  options.adapter.custom.to_value = wh::compose::edge_to_value_adapter{
      [](wh::compose::graph_stream_reader reader, const wh::compose::edge_limits &,
         wh::core::run_context &) -> wh::compose::graph_value_sender {
        return wh::compose::detail::bridge_graph_sender(
            stdexec::just(std::move(reader)) |
            stdexec::then([](wh::compose::graph_stream_reader source)
                              -> wh::core::result<wh::compose::graph_value> {
              auto chunks = wh::compose::collect_graph_stream_reader(std::move(source));
              if (chunks.has_error()) {
                return wh::core::result<wh::compose::graph_value>::failure(chunks.error());
              }
              return wh::core::any(std::move(chunks).value());
            }));
      }};
  return options;
}

[[nodiscard]] auto build_lowering_graph(const source_shape source, const target_shape target,
                                        const lowering_style style,
                                        const wh::compose::graph_runtime_mode mode,
                                        const std::size_t parallel)
    -> wh::core::result<wh::compose::graph> {
  wh::compose::graph_compile_options options{};
  options.mode = mode;
  options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
  options.fan_in_policy = wh::compose::graph_fan_in_policy::require_all_sources;
  options.max_parallel_nodes = parallel;
  options.max_parallel_per_node = parallel;
  wh::compose::graph graph{std::move(options)};

  switch (source) {
  case source_shape::single_value: {
    auto status = add_value_source(graph, "src", 1);
    if (status.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(status.error());
    }
    break;
  }
  case source_shape::single_stream: {
    auto status = add_stream_source(graph, "src", 1);
    if (status.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(status.error());
    }
    break;
  }
  case source_shape::multi_value: {
    auto left = add_value_source(graph, "a", 1);
    if (left.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(left.error());
    }
    auto right = add_value_source(graph, "b", 10);
    if (right.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(right.error());
    }
    break;
  }
  case source_shape::multi_stream: {
    auto left = add_stream_source(graph, "a", 1);
    if (left.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(left.error());
    }
    auto right = add_stream_source(graph, "b", 10);
    if (right.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(right.error());
    }
    break;
  }
  case source_shape::mixed: {
    auto value_status = add_value_source(graph, "a", 1);
    if (value_status.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(value_status.error());
    }
    auto stream_status = add_stream_source(graph, "b", 10);
    if (stream_status.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(stream_status.error());
    }
    break;
  }
  }

  switch (target) {
  case target_shape::single_value: {
    auto status = add_value_sink(graph, "sink", 100);
    if (status.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(status.error());
    }
    break;
  }
  case target_shape::single_stream: {
    auto status = add_stream_sink(graph, "sink", 200);
    if (status.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(status.error());
    }
    break;
  }
  case target_shape::multi_value: {
    auto left = add_value_sink(graph, "left", 10);
    if (left.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(left.error());
    }
    auto right = add_value_sink(graph, "right", 20);
    if (right.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(right.error());
    }
    break;
  }
  case target_shape::multi_stream: {
    auto left = add_stream_sink(graph, "left", 10);
    if (left.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(left.error());
    }
    auto right = add_stream_sink(graph, "right", 20);
    if (right.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(right.error());
    }
    break;
  }
  }

  if (target == target_shape::multi_value || target == target_shape::multi_stream) {
    auto added = graph.add_lambda(
        "join",
        [](const wh::compose::graph_value &input, wh::core::run_context &,
           const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
          auto merged = read_any<wh::compose::graph_value_map>(input);
          if (merged.has_error()) {
            return wh::core::result<wh::compose::graph_value>::failure(merged.error());
          }
          auto left = read_named_int(merged.value(), "left");
          auto right = read_named_int(merged.value(), "right");
          if (left.has_error()) {
            return wh::core::result<wh::compose::graph_value>::failure(left.error());
          }
          if (right.has_error()) {
            return wh::core::result<wh::compose::graph_value>::failure(right.error());
          }
          return wh::core::any(left.value() + right.value());
        });
    if (added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(added.error());
    }
  }

  const auto input_contract_of =
      [&](const std::string_view key) noexcept -> wh::compose::node_contract {
    if (key == wh::compose::graph_start_node_key) {
      return wh::compose::node_contract::value;
    }
    if (key == wh::compose::graph_end_node_key) {
      return wh::compose::node_contract::value;
    }
    if (key == "src" || key == "a" || key == "b") {
      return wh::compose::node_contract::value;
    }
    if (key == "sink") {
      return target == target_shape::single_stream ? wh::compose::node_contract::stream
                                                   : wh::compose::node_contract::value;
    }
    if (key == "left" || key == "right") {
      return target == target_shape::multi_stream ? wh::compose::node_contract::stream
                                                  : wh::compose::node_contract::value;
    }
    return wh::compose::node_contract::value;
  };

  const auto output_contract_of =
      [&](const std::string_view key) noexcept -> wh::compose::node_contract {
    if (key == wh::compose::graph_start_node_key) {
      return wh::compose::node_contract::value;
    }
    if (key == wh::compose::graph_end_node_key) {
      return wh::compose::node_contract::value;
    }
    if (key == "sink" || key == "left" || key == "right" || key == "join") {
      return wh::compose::node_contract::value;
    }
    if (key == "src") {
      return source == source_shape::single_stream ? wh::compose::node_contract::stream
                                                   : wh::compose::node_contract::value;
    }
    switch (source) {
    case source_shape::single_value:
    case source_shape::multi_value:
      return wh::compose::node_contract::value;
    case source_shape::single_stream:
    case source_shape::multi_stream:
      return wh::compose::node_contract::stream;
    case source_shape::mixed:
      return key == "b" ? wh::compose::node_contract::stream : wh::compose::node_contract::value;
    }
    return wh::compose::node_contract::value;
  };

  const auto connect = [&](const std::string_view from,
                           const std::string_view to) -> wh::core::result<void> {
    const auto source_stream = output_contract_of(from) == wh::compose::node_contract::stream;
    const auto target_stream = input_contract_of(to) == wh::compose::node_contract::stream;
    auto edge_options = style == lowering_style::builtin
                            ? make_builtin_bridge_options(source_stream, target_stream)
                            : make_custom_bridge_options(source_stream, target_stream);
    if (!edge_options.has_value()) {
      auto edge = graph.add_edge(std::string{from}, std::string{to});
      if (edge.has_error()) {
        return wh::core::result<void>::failure(edge.error());
      }
      return {};
    }

    auto edge = graph.add_edge(std::string{from}, std::string{to}, std::move(*edge_options));
    if (edge.has_error()) {
      return wh::core::result<void>::failure(edge.error());
    }
    return {};
  };

  const auto connect_source = [&](std::string_view key) -> wh::core::result<void> {
    auto entry = connect(wh::compose::graph_start_node_key, key);
    if (entry.has_error()) {
      return entry;
    }
    if (target == target_shape::single_value || target == target_shape::single_stream) {
      return connect(key, "sink");
    }
    auto left = connect(key, "left");
    if (left.has_error()) {
      return left;
    }
    auto right = connect(key, "right");
    if (right.has_error()) {
      return right;
    }
    return {};
  };

  switch (source) {
  case source_shape::single_value:
  case source_shape::single_stream: {
    auto status = connect_source("src");
    if (status.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(status.error());
    }
    break;
  }
  case source_shape::multi_value:
  case source_shape::multi_stream:
  case source_shape::mixed: {
    auto left = connect_source("a");
    if (left.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(left.error());
    }
    auto right = connect_source("b");
    if (right.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(right.error());
    }
    break;
  }
  }

  if (target == target_shape::single_value || target == target_shape::single_stream) {
    auto finish = connect("sink", wh::compose::graph_end_node_key);
    if (finish.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(finish.error());
    }
  } else {
    auto left = connect("left", "join");
    if (left.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(left.error());
    }
    auto right = connect("right", "join");
    if (right.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(right.error());
    }
    auto finish = connect("join", wh::compose::graph_end_node_key);
    if (finish.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(finish.error());
    }
  }

  auto compiled = graph.compile();
  if (compiled.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(compiled.error());
  }
  return graph;
}

[[nodiscard]] constexpr auto expected_lowering_output(const source_shape source,
                                                      const target_shape target,
                                                      const int iteration) noexcept -> int {
  const auto total = base_sum(source, iteration);
  switch (target) {
  case target_shape::single_value:
    return total + 100;
  case target_shape::single_stream:
    return total + 200;
  case target_shape::multi_value:
  case target_shape::multi_stream:
    return 2 * total + 30;
  }
  return 0;
}

struct stream_observation {
  int sum{0};
  int source_count{0};
};

[[nodiscard]] auto observe_stream(wh::compose::graph_stream_reader reader)
    -> wh::core::result<stream_observation> {
  std::unordered_set<std::string> sources{};
  int total = 0;
  while (true) {
    auto next = reader.read();
    if (next.has_error()) {
      return wh::core::result<stream_observation>::failure(next.error());
    }
    auto chunk = std::move(next).value();
    if (chunk.error != wh::core::errc::ok) {
      return wh::core::result<stream_observation>::failure(chunk.error);
    }
    if (chunk.is_terminal_eof()) {
      break;
    }
    if (chunk.is_source_eof()) {
      continue;
    }
    if (!chunk.source.empty()) {
      sources.insert(chunk.source);
    }
    if (chunk.value.has_value()) {
      auto typed = read_any<int>(std::move(*chunk.value));
      if (typed.has_error()) {
        return wh::core::result<stream_observation>::failure(typed.error());
      }
      total += typed.value();
    }
  }
  return stream_observation{.sum = total, .source_count = static_cast<int>(sources.size())};
}

[[nodiscard]] auto parse_int_text(const std::string_view text) -> wh::core::result<int> {
  try {
    return std::stoi(std::string{text});
  } catch (...) {
    return wh::core::result<int>::failure(wh::core::errc::type_mismatch);
  }
}

[[nodiscard]] auto read_message_text_int(const wh::schema::message &message)
    -> wh::core::result<int> {
  if (message.parts.empty()) {
    return wh::core::result<int>::failure(wh::core::errc::not_found);
  }
  const auto *text = std::get_if<wh::schema::text_part>(&message.parts.front());
  if (text == nullptr) {
    return wh::core::result<int>::failure(wh::core::errc::type_mismatch);
  }
  return parse_int_text(text->text);
}

[[nodiscard]] auto shift_int_stream(wh::compose::graph_stream_reader input, const int delta)
    -> wh::core::result<wh::compose::graph_stream_reader> {
  auto values = collect_int_graph_stream(std::move(input));
  if (values.has_error()) {
    return wh::core::result<wh::compose::graph_stream_reader>::failure(values.error());
  }
  std::vector<wh::compose::graph_value> output{};
  output.reserve(values.value().size());
  for (const auto value : values.value()) {
    output.emplace_back(wh::core::any(value + delta));
  }
  return wh::compose::make_values_stream_reader(std::move(output));
}

[[nodiscard]] auto sum_int_stream(wh::compose::graph_stream_reader input) -> wh::core::result<int> {
  auto values = collect_int_graph_stream(std::move(input));
  if (values.has_error()) {
    return wh::core::result<int>::failure(values.error());
  }
  return sum_ints(values.value());
}

[[nodiscard]] constexpr auto case_from(const node_case value) noexcept
    -> wh::compose::node_contract {
  return describe_node_case(value).from;
}

[[nodiscard]] constexpr auto case_to(const node_case value) noexcept -> wh::compose::node_contract {
  return describe_node_case(value).to;
}

[[nodiscard]] auto parse_prefixed_int(const std::string_view text, const std::string_view prefix)
    -> wh::core::result<int> {
  if (!text.starts_with(prefix)) {
    return wh::core::result<int>::failure(wh::core::errc::type_mismatch);
  }
  return parse_int_text(text.substr(prefix.size()));
}

[[nodiscard]] auto parse_pair_suffix(const std::string_view text) -> wh::core::result<int> {
  const auto separator = text.find(':');
  if (separator == std::string_view::npos) {
    return parse_int_text(text);
  }

  auto head = parse_int_text(text.substr(0U, separator));
  if (head.has_error()) {
    return wh::core::result<int>::failure(head.error());
  }

  const auto tail_text = text.substr(separator + 1U);
  auto tail = parse_int_text(tail_text);
  if (tail.has_error()) {
    if (tail_text == "a") {
      tail = 1;
    } else if (tail_text == "b") {
      tail = 2;
    } else {
      return wh::core::result<int>::failure(tail.error());
    }
  }

  return head.value() + tail.value();
}

[[nodiscard]] auto adapt_case_input(const node_case value, const wh::compose::graph_value &input)
    -> wh::core::result<wh::compose::graph_value> {
  auto base = sum_graph_value(input);
  if (base.has_error()) {
    return wh::core::result<wh::compose::graph_value>::failure(base.error());
  }

  switch (value) {
  case node_case::component_embedding_vv:
  case node_case::component_embedding_vv_async:
    return wh::core::any(wh::embedding::embedding_request{
        .inputs = std::vector<std::string>{std::to_string(base.value())}});
  case node_case::component_model_vv:
  case node_case::component_model_vs:
  case node_case::component_model_vv_async:
  case node_case::component_model_vs_async:
    return wh::core::any(make_chat_request(std::to_string(base.value())));
  case node_case::component_prompt_vv:
  case node_case::component_prompt_vv_async:
    return wh::core::any(make_prompt_request(std::to_string(base.value())));
  case node_case::component_retriever_vv:
  case node_case::component_retriever_vv_async:
    return wh::core::any(
        wh::retriever::retriever_request{.query = std::to_string(base.value()), .index = "idx"});
  case node_case::component_indexer_vv:
  case node_case::component_indexer_vv_async:
    return wh::core::any(wh::indexer::indexer_request{
        .documents = {wh::schema::document{std::to_string(base.value())}}});
  case node_case::component_document_vv:
  case node_case::component_document_vv_async:
    return wh::core::any(make_document_request(std::to_string(base.value())));
  case node_case::component_tool_vv:
  case node_case::component_tool_vs:
  case node_case::component_tool_vv_async:
  case node_case::component_tool_vs_async:
    return wh::core::any(wh::tool::tool_request{.input_json = std::to_string(base.value())});
  case node_case::tools_vv_sync:
  case node_case::tools_vs_sync:
  case node_case::tools_vv_async:
  case node_case::tools_vs_async:
    return wh::core::any(make_tool_batch({wh::compose::tool_call{
        .call_id = "call",
        .tool_name = "echo",
        .arguments = std::to_string(base.value()),
    }}));
  default:
    return wh::core::any(base.value());
  }
}

[[nodiscard]] auto reduce_case_value(const node_case value, const wh::compose::graph_value &output)
    -> wh::core::result<int> {
  switch (value) {
  case node_case::component_embedding_vv:
  case node_case::component_embedding_vv_async: {
    auto typed = read_any<wh::embedding::embedding_response>(output);
    if (typed.has_error()) {
      return wh::core::result<int>::failure(typed.error());
    }
    if (typed.value().empty() || typed.value().front().empty()) {
      return wh::core::result<int>::failure(wh::core::errc::not_found);
    }
    return static_cast<int>(typed.value().front().front());
  }
  case node_case::component_model_vv:
  case node_case::component_model_vv_async: {
    auto typed = read_any<wh::model::chat_response>(output);
    if (typed.has_error()) {
      return wh::core::result<int>::failure(typed.error());
    }
    return read_message_text_int(typed.value().message);
  }
  case node_case::component_prompt_vv:
  case node_case::component_prompt_vv_async: {
    auto typed = read_any<std::vector<wh::schema::message>>(output);
    if (typed.has_error()) {
      return wh::core::result<int>::failure(typed.error());
    }
    if (typed.value().empty()) {
      return wh::core::result<int>::failure(wh::core::errc::not_found);
    }
    return parse_prefixed_int(
        std::get<wh::schema::text_part>(typed.value().front().parts.front()).text, "Hello ");
  }
  case node_case::component_retriever_vv:
  case node_case::component_retriever_vv_async: {
    auto typed = read_any<wh::retriever::retriever_response>(output);
    if (typed.has_error()) {
      return wh::core::result<int>::failure(typed.error());
    }
    if (typed.value().empty()) {
      return wh::core::result<int>::failure(wh::core::errc::not_found);
    }
    return parse_prefixed_int(typed.value().front().content(), "hit:");
  }
  case node_case::component_indexer_vv:
  case node_case::component_indexer_vv_async: {
    auto typed = read_any<wh::indexer::indexer_response>(output);
    if (typed.has_error()) {
      return wh::core::result<int>::failure(typed.error());
    }
    return static_cast<int>(typed.value().success_count);
  }
  case node_case::component_document_vv:
  case node_case::component_document_vv_async: {
    auto typed = read_any<wh::document::document_batch>(output);
    if (typed.has_error()) {
      return wh::core::result<int>::failure(typed.error());
    }
    if (typed.value().empty()) {
      return wh::core::result<int>::failure(wh::core::errc::not_found);
    }
    return parse_int_text(typed.value().front().content());
  }
  case node_case::component_tool_vv:
  case node_case::component_tool_vv_async: {
    auto typed = read_any<std::string>(output);
    if (typed.has_error()) {
      return wh::core::result<int>::failure(typed.error());
    }
    return parse_prefixed_int(typed.value(), "tool:");
  }
  case node_case::tools_vv_sync: {
    auto results = collect_tool_results(output);
    if (results.has_error() || results.value().get().empty()) {
      return wh::core::result<int>::failure(results.has_error() ? results.error()
                                                                : wh::core::errc::not_found);
    }
    auto typed = read_any<std::string>(results.value().get().front().value);
    if (typed.has_error()) {
      return wh::core::result<int>::failure(typed.error());
    }
    return parse_prefixed_int(typed.value(), "sync:");
  }
  case node_case::tools_vv_async: {
    auto results = collect_tool_results(output);
    if (results.has_error() || results.value().get().empty()) {
      return wh::core::result<int>::failure(results.has_error() ? results.error()
                                                                : wh::core::errc::not_found);
    }
    auto typed = read_any<std::string>(results.value().get().front().value);
    if (typed.has_error()) {
      return wh::core::result<int>::failure(typed.error());
    }
    return parse_prefixed_int(typed.value(), "async:");
  }
  default:
    return read_any<int>(output);
  }
}

[[nodiscard]] auto reduce_case_stream_item(const node_case value, wh::compose::graph_value item)
    -> wh::core::result<int> {
  switch (value) {
  case node_case::component_model_vs:
  case node_case::component_model_vs_async: {
    auto typed = read_any<wh::schema::message>(std::move(item));
    if (typed.has_error()) {
      return wh::core::result<int>::failure(typed.error());
    }
    return read_message_text_int(typed.value());
  }
  case node_case::component_tool_vs:
  case node_case::component_tool_vs_async: {
    auto typed = read_any<std::string>(std::move(item));
    if (typed.has_error()) {
      return wh::core::result<int>::failure(typed.error());
    }
    return parse_int_text(typed.value());
  }
  case node_case::tools_vs_sync:
  case node_case::tools_vs_async: {
    auto typed = read_any<wh::compose::tool_event>(std::move(item));
    if (typed.has_error()) {
      return wh::core::result<int>::failure(typed.error());
    }
    auto text = read_any<std::string>(std::move(typed.value().value));
    if (text.has_error()) {
      return wh::core::result<int>::failure(text.error());
    }
    return parse_pair_suffix(text.value());
  }
  default:
    return read_any<int>(std::move(item));
  }
}

[[nodiscard]] auto reduce_case_stream(const node_case value,
                                      wh::compose::graph_stream_reader output)
    -> wh::core::result<int> {
  auto values = wh::compose::collect_graph_stream_reader(std::move(output));
  if (values.has_error()) {
    return wh::core::result<int>::failure(values.error());
  }

  int total = 0;
  for (auto &entry : values.value()) {
    auto mapped = reduce_case_stream_item(value, std::move(entry));
    if (mapped.has_error()) {
      return wh::core::result<int>::failure(mapped.error());
    }
    total += mapped.value();
  }
  return total;
}

[[nodiscard]] auto make_block_stream_input(const wh::compose::graph_value &input)
    -> wh::core::result<wh::compose::graph_stream_reader> {
  auto base = sum_graph_value(input);
  if (base.has_error()) {
    return wh::core::result<wh::compose::graph_stream_reader>::failure(base.error());
  }
  return make_int_graph_stream({base.value(), base.value() + 1, base.value() + 2});
}

[[nodiscard]] auto add_case_subgraph(wh::compose::graph &graph, const std::string &key,
                                     const node_case value, const wh::compose::graph &child)
    -> wh::core::result<void> {
  (void)value;
  return graph.add_subgraph(wh::compose::make_subgraph_node(key, child));
}

[[nodiscard]] auto append_case_block(wh::compose::graph &graph, const std::string &key,
                                     const wh::compose::graph &child) -> wh::core::result<void> {
  return graph.add_subgraph(wh::compose::make_subgraph_node(key, child));
}

struct block_case_runtime {
  node_case kind{};
  wh::compose::graph case_graph{};
  compiled_graph_node_view case_node{};
  wh::compose::graph block_graph{};
};

[[nodiscard]] auto make_case_block(const node_case value,
                                   const wh::compose::graph_runtime_mode mode,
                                   const wh::compose::graph &actual)
    -> wh::core::result<wh::compose::graph> {
  wh::compose::graph_compile_options options{};
  options.mode = mode;
  wh::compose::graph graph{std::move(options)};

  const auto actual_key = std::string{"actual"};
  auto added_actual = add_case_subgraph(graph, actual_key, value, actual);
  if (added_actual.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added_actual.error());
  }

  if (case_from(value) == wh::compose::node_contract::value) {
    auto added = graph.add_lambda(
        "pre",
        [value](
            const wh::compose::graph_value &input, wh::core::run_context &,
            const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
          return adapt_case_input(value, input);
        });
    if (added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(added.error());
    }
    if (auto start = graph.add_entry_edge("pre"); start.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(start.error());
    }
    if (auto link = graph.add_edge("pre", actual_key); link.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(link.error());
    }
  } else {
    auto added =
        graph.add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream>(
            "pre",
            [](const wh::compose::graph_value &input, wh::core::run_context &,
               const wh::compose::graph_call_scope &)
                -> wh::core::result<wh::compose::graph_stream_reader> {
              return make_block_stream_input(input);
            });
    if (added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(added.error());
    }
    if (auto start = graph.add_entry_edge("pre"); start.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(start.error());
    }
    if (auto link = graph.add_edge("pre", actual_key); link.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(link.error());
    }
  }

  if (case_to(value) == wh::compose::node_contract::value) {
    auto added = graph.add_lambda(
        "post",
        [value](
            const wh::compose::graph_value &input, wh::core::run_context &,
            const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
          auto reduced = reduce_case_value(value, input);
          if (reduced.has_error()) {
            return wh::core::result<wh::compose::graph_value>::failure(reduced.error());
          }
          return wh::core::any(reduced.value());
        });
    if (added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(added.error());
    }
    if (auto link = graph.add_edge(actual_key, "post"); link.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(link.error());
    }
    if (auto finish = graph.add_exit_edge("post"); finish.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(finish.error());
    }
  } else {
    auto added =
        graph.add_lambda<wh::compose::node_contract::stream, wh::compose::node_contract::value>(
            "post",
            [value](wh::compose::graph_stream_reader input, wh::core::run_context &,
                    const wh::compose::graph_call_scope &)
                -> wh::core::result<wh::compose::graph_value> {
              auto reduced = reduce_case_stream(value, std::move(input));
              if (reduced.has_error()) {
                return wh::core::result<wh::compose::graph_value>::failure(reduced.error());
              }
              return wh::core::any(reduced.value());
            });
    if (added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(added.error());
    }
    if (auto link = graph.add_edge(actual_key, "post"); link.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(link.error());
    }
    if (auto finish = graph.add_exit_edge("post"); finish.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(finish.error());
    }
  }

  auto compiled = graph.compile();
  if (compiled.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(compiled.error());
  }
  return graph;
}

[[nodiscard]] auto prepare_block_cases(const wh::compose::graph_runtime_mode mode)
    -> wh::core::result<std::vector<block_case_runtime>> {
  std::vector<block_case_runtime> blocks{};
  blocks.reserve(all_node_cases().size());

  for (const auto item : all_node_cases()) {
    auto case_graph = build_node_graph(item, mode);
    if (case_graph.has_error()) {
      return wh::core::result<std::vector<block_case_runtime>>::failure(case_graph.error());
    }
    auto block_graph = make_case_block(item, mode, case_graph.value());
    if (block_graph.has_error()) {
      return wh::core::result<std::vector<block_case_runtime>>::failure(block_graph.error());
    }

    blocks.emplace_back(block_case_runtime{
        .kind = item,
        .case_graph = std::move(case_graph).value(),
        .block_graph = std::move(block_graph).value(),
    });
    auto compiled = compile_test_graph_node(blocks.back().case_graph);
    if (compiled.has_error()) {
      return wh::core::result<std::vector<block_case_runtime>>::failure(compiled.error());
    }
    blocks.back().case_node = compiled.value();
  }

  return blocks;
}

[[nodiscard]] auto evaluate_case_block(const block_case_runtime &block, const int input_value)
    -> wh::core::result<wh::compose::graph_value> {
  wh::compose::graph_value input = wh::core::any(input_value);
  auto prepared_input = case_from(block.kind) == wh::compose::node_contract::value
                            ? adapt_case_input(block.kind, input)
                            : [&input]() -> wh::core::result<wh::compose::graph_value> {
    auto reader = make_block_stream_input(input);
    if (reader.has_error()) {
      return wh::core::result<wh::compose::graph_value>::failure(reader.error());
    }
    return wh::core::any(std::move(reader).value());
  }();
  if (prepared_input.has_error()) {
    return wh::core::result<wh::compose::graph_value>::failure(prepared_input.error());
  }

  wh::core::run_context context{};
  auto actual = wait_sender_result<wh::core::result<wh::compose::graph_value>>(
      run_compiled_test_graph_node(block.case_node, std::move(prepared_input).value(), context));
  if (actual.has_error()) {
    return wh::core::result<wh::compose::graph_value>::failure(actual.error());
  }

  if (case_to(block.kind) == wh::compose::node_contract::value) {
    auto reduced = reduce_case_value(block.kind, actual.value());
    if (reduced.has_error()) {
      return wh::core::result<wh::compose::graph_value>::failure(reduced.error());
    }
    return wh::core::any(reduced.value());
  }

  auto reader = read_any<wh::compose::graph_stream_reader>(std::move(actual).value());
  if (reader.has_error()) {
    return wh::core::result<wh::compose::graph_value>::failure(reader.error());
  }
  auto reduced = reduce_case_stream(block.kind, std::move(reader).value());
  if (reduced.has_error()) {
    return wh::core::result<wh::compose::graph_value>::failure(reduced.error());
  }
  return wh::core::any(reduced.value());
}

[[nodiscard]] auto
make_block_chain(const std::vector<std::reference_wrapper<const wh::compose::graph>> &blocks,
                 const wh::compose::graph_runtime_mode mode)
    -> wh::core::result<wh::compose::graph> {
  wh::compose::graph_compile_options options{};
  options.mode = mode;
  wh::compose::graph graph{std::move(options)};

  std::string previous = std::string{wh::compose::graph_start_node_key};
  for (std::size_t index = 0U; index < blocks.size(); ++index) {
    const auto key = std::string{"block_"} + std::to_string(index);
    auto added = append_case_block(graph, key, blocks[index].get());
    if (added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(added.error());
    }
    auto edge = graph.add_edge(previous, key);
    if (edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(edge.error());
    }
    previous = key;
  }

  auto finish = graph.add_exit_edge(previous);
  if (finish.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(finish.error());
  }
  auto compiled = graph.compile();
  if (compiled.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(compiled.error());
  }
  return graph;
}

inline auto expect_same_block_output(wh::compose::graph_value actual,
                                     wh::compose::graph_value expected) -> void {
  auto actual_value = read_any<int>(std::move(actual));
  REQUIRE(actual_value.has_value());
  auto expected_value = read_any<int>(std::move(expected));
  REQUIRE(expected_value.has_value());
  REQUIRE(actual_value.value() == expected_value.value());
}

inline auto run_pair_matrix_shard(const std::size_t shard, const std::size_t shard_count) -> void {
  constexpr std::array iterations{0, 7};

  for (const auto mode : runtime_modes()) {
    auto blocks = prepare_block_cases(mode);
    REQUIRE(blocks.has_value());

    const auto pair_count = blocks.value().size() * blocks.value().size();
    for (std::size_t pair_index = 0U; pair_index < pair_count; ++pair_index) {
      if ((pair_index % shard_count) != shard) {
        continue;
      }
      const auto left_index = pair_index / blocks.value().size();
      const auto right_index = pair_index % blocks.value().size();
      const auto &left = blocks.value()[left_index];
      const auto &right = blocks.value()[right_index];

      DYNAMIC_SECTION(runtime_mode_name(mode)
                      << ":shard=" << shard << ":" << node_case_name(left.kind) << "->"
                      << node_case_name(right.kind)) {
        auto graph =
            make_block_chain({std::cref(left.block_graph), std::cref(right.block_graph)}, mode);
        REQUIRE(graph.has_value());

        for (const auto iteration : iterations) {
          wh::core::run_context actual_context{};
          auto actual = invoke_value_sync(graph.value(), wh::core::any(iteration), actual_context);
          REQUIRE(actual.has_value());

          auto expected_left = evaluate_case_block(left, iteration);
          REQUIRE(expected_left.has_value());
          auto left_value = read_any<int>(std::move(expected_left).value());
          REQUIRE(left_value.has_value());

          auto expected_right = evaluate_case_block(right, left_value.value());
          REQUIRE(expected_right.has_value());

          expect_same_block_output(std::move(actual).value(), std::move(expected_right).value());
        }
      }
    }
  }
}

struct stream_shift_component {
  int delta{0};

  [[nodiscard]] auto stream(wh::compose::graph_stream_reader input, wh::core::run_context &) const
      -> wh::core::result<wh::compose::graph_stream_reader> {
    return shift_int_stream(std::move(input), delta);
  }
};

[[nodiscard]] auto make_value_subgraph(const int delta) -> wh::core::result<wh::compose::graph> {
  wh::compose::graph child{};
  auto added = child.add_lambda(
      "leaf",
      [delta](const wh::compose::graph_value &input, wh::core::run_context &,
              const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
        auto typed = read_any<int>(input);
        if (typed.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(typed.error());
        }
        return wh::core::any(typed.value() + delta);
      });
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }
  if (auto start = child.add_entry_edge("leaf"); start.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(start.error());
  }
  if (auto finish = child.add_exit_edge("leaf"); finish.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(finish.error());
  }
  auto compiled = child.compile();
  if (compiled.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(compiled.error());
  }
  return child;
}

TEST_CASE("compose node family stress matrix remains stable", "[core][compose][node][stress]") {
  for (const auto mode : runtime_modes()) {
    for (const auto item : all_node_cases()) {
      DYNAMIC_SECTION(runtime_mode_name(mode) << ":" << node_case_name(item)) {
        auto graph = build_node_graph(item, mode);
        REQUIRE(graph.has_value());
        auto compiled = compile_test_graph_node(graph.value());
        REQUIRE(compiled.has_value());

        for (int iteration = 0; iteration < 8; ++iteration) {
          auto input = make_node_input(item, iteration);
          REQUIRE(input.has_value());
          wh::core::run_context context{};
          auto output = wait_sender_result<wh::core::result<wh::compose::graph_value>>(
              run_compiled_test_graph_node(compiled.value(), std::move(input).value(), context));
          REQUIRE(output.has_value());
          verify_node_output(item, std::move(output).value(), iteration);
        }
      }
    }
  }
}

TEST_CASE("compose graph runtime preserves async lambda contracts",
          "[core][compose][graph][runtime]") {
  constexpr std::array async_cases{
      node_case::lambda_vv_async,
      node_case::lambda_vs_async,
      node_case::lambda_sv_async,
      node_case::lambda_ss_async,
  };

  for (const auto mode : runtime_modes()) {
    for (const auto item : async_cases) {
      DYNAMIC_SECTION(runtime_mode_name(mode) << ":" << node_case_name(item)) {
        auto graph = build_node_graph(item, mode);
        REQUIRE(graph.has_value());

        auto input = make_node_input(item, 7);
        REQUIRE(input.has_value());

        wh::core::run_context context{};
        auto actual = invoke_value_sync(graph.value(), std::move(input).value(), context);
        REQUIRE(actual.has_value());
        verify_node_output(item, std::move(actual).value(), 7);
      }
    }
  }
}

TEST_CASE("compose graph runtime preserves async lambda map contracts",
          "[core][compose][graph][runtime]") {
  for (const auto mode : runtime_modes()) {
    DYNAMIC_SECTION(runtime_mode_name(mode)) {
      wh::compose::graph_compile_options options{};
      options.mode = mode;
      wh::compose::graph graph{std::move(options)};
      REQUIRE(
          graph
              .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::value,
                          wh::compose::node_exec_mode::async>(
                  "worker",
                  [](wh::compose::graph_value_map &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &) {
                    auto left = read_named_int(input, "left");
                    auto right = read_named_int(input, "right");
                    if (left.has_error()) {
                      return stdexec::just(
                          wh::core::result<wh::compose::graph_value_map>::failure(left.error()));
                    }
                    if (right.has_error()) {
                      return stdexec::just(
                          wh::core::result<wh::compose::graph_value_map>::failure(right.error()));
                    }
                    wh::compose::graph_value_map output{};
                    output.emplace("sum", wh::core::any(left.value() + right.value()));
                    output.emplace("product", wh::core::any(left.value() * right.value()));
                    return stdexec::just(
                        wh::core::result<wh::compose::graph_value_map>{std::move(output)});
                  })
              .has_value());
      REQUIRE(graph.add_entry_edge("worker").has_value());
      REQUIRE(graph.add_exit_edge("worker").has_value());
      REQUIRE(graph.compile().has_value());

      wh::compose::graph_value_map input{};
      input.emplace("left", wh::core::any(3));
      input.emplace("right", wh::core::any(5));

      wh::core::run_context context{};
      auto output = invoke_value_sync(graph, wh::core::any(std::move(input)), context);
      REQUIRE(output.has_value());

      auto typed = read_any<wh::compose::graph_value_map>(output.value());
      REQUIRE(typed.has_value());
      REQUIRE(read_named_int(typed.value(), "sum").value() == 8);
      REQUIRE(read_named_int(typed.value(), "product").value() == 15);
    }
  }
}

TEST_CASE("compose graph runtime preserves async lambda move-only inputs",
          "[core][compose][graph][runtime]") {
  struct move_only_payload {
    int value{0};

    move_only_payload() = default;
    explicit move_only_payload(const int current) : value(current) {}
    move_only_payload(move_only_payload &&) noexcept = default;
    auto operator=(move_only_payload &&) noexcept -> move_only_payload & = default;
    move_only_payload(const move_only_payload &) = delete;
    auto operator=(const move_only_payload &) -> move_only_payload & = delete;
  };

  for (const auto mode : runtime_modes()) {
    DYNAMIC_SECTION(runtime_mode_name(mode)) {
      wh::compose::graph_compile_options options{};
      options.mode = mode;
      wh::compose::graph graph{std::move(options)};
      REQUIRE(graph
                  .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::value,
                              wh::compose::node_exec_mode::async>(
                      "worker",
                      [](wh::compose::graph_value &input, wh::core::run_context &,
                         const wh::compose::graph_call_scope &) {
                        auto payload = read_any<move_only_payload>(std::move(input));
                        if (payload.has_error()) {
                          return stdexec::just(
                              wh::core::result<wh::compose::graph_value>::failure(payload.error()));
                        }
                        return stdexec::just(wh::core::result<wh::compose::graph_value>{
                            wh::core::any(payload.value().value + 9)});
                      })
                  .has_value());
      REQUIRE(graph.add_entry_edge("worker").has_value());
      REQUIRE(graph.add_exit_edge("worker").has_value());
      REQUIRE(graph.compile().has_value());

      wh::core::run_context context{};
      auto output = invoke_value_sync(graph, wh::core::any(move_only_payload{12}), context);
      REQUIRE(output.has_value());
      auto typed = read_any<int>(output.value());
      REQUIRE(typed.has_value());
      REQUIRE(typed.value() == 21);
    }
  }
}

TEST_CASE("compose graph runtime rejects move-only value fanout to multiple async consumers",
          "[core][compose][graph][runtime]") {
  struct move_only_payload {
    int value{0};

    move_only_payload() = default;
    explicit move_only_payload(const int current) : value(current) {}
    move_only_payload(move_only_payload &&) noexcept = default;
    auto operator=(move_only_payload &&) noexcept -> move_only_payload & = default;
    move_only_payload(const move_only_payload &) = delete;
    auto operator=(const move_only_payload &) -> move_only_payload & = delete;
  };

  for (const auto mode : runtime_modes()) {
    DYNAMIC_SECTION(runtime_mode_name(mode)) {
      wh::compose::graph_compile_options options{};
      options.mode = mode;
      wh::compose::graph graph{std::move(options)};
      std::atomic<int> executions{0};
      const auto make_worker = [&executions](const int delta) {
        return [delta, &executions](wh::compose::graph_value &input, wh::core::run_context &,
                                    const wh::compose::graph_call_scope &) {
          executions.fetch_add(1, std::memory_order_relaxed);
          auto payload = read_any<move_only_payload>(std::move(input));
          if (payload.has_error()) {
            return stdexec::just(
                wh::core::result<wh::compose::graph_value>::failure(payload.error()));
          }
          return stdexec::just(wh::core::result<wh::compose::graph_value>{
              wh::core::any(payload.value().value + delta)});
        };
      };
      REQUIRE(graph
                  .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::value,
                              wh::compose::node_exec_mode::async>("left", make_worker(1))
                  .has_value());
      REQUIRE(graph
                  .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::value,
                              wh::compose::node_exec_mode::async>("right", make_worker(2))
                  .has_value());
      REQUIRE(graph.add_entry_edge("left").has_value());
      REQUIRE(graph.add_entry_edge("right").has_value());
      REQUIRE(graph.add_exit_edge("left").has_value());
      REQUIRE(graph.add_exit_edge("right").has_value());
      REQUIRE(graph.compile().has_value());

      wh::core::run_context context{};
      auto output = invoke_value_sync(graph, wh::core::any(move_only_payload{12}), context);
      REQUIRE(output.has_error());
      REQUIRE(output.error() == wh::core::errc::contract_violation);
      REQUIRE(executions.load(std::memory_order_relaxed) == 0);
    }
  }
}

TEST_CASE("compose node block pair matrix shard 0 remains stable",
          "[core][compose][graph][stress][pair]") {
  run_pair_matrix_shard(0U, 4U);
}

TEST_CASE("compose node block pair matrix shard 1 remains stable",
          "[core][compose][graph][stress][pair]") {
  run_pair_matrix_shard(1U, 4U);
}

TEST_CASE("compose node block pair matrix shard 2 remains stable",
          "[core][compose][graph][stress][pair]") {
  run_pair_matrix_shard(2U, 4U);
}

TEST_CASE("compose node block pair matrix shard 3 remains stable",
          "[core][compose][graph][stress][pair]") {
  run_pair_matrix_shard(3U, 4U);
}

TEST_CASE("compose node block chain length matrix remains stable",
          "[core][compose][graph][stress][chain_length]") {
  constexpr std::array lengths{std::size_t{2U}, std::size_t{3U}, std::size_t{5U}};

  for (const auto mode : runtime_modes()) {
    auto blocks = prepare_block_cases(mode);
    REQUIRE(blocks.has_value());

    for (const auto length : lengths) {
      for (std::size_t start = 0U; start < blocks.value().size(); ++start) {
        DYNAMIC_SECTION(runtime_mode_name(mode) << ":length=" << length << ":start=" << start) {
          std::vector<std::reference_wrapper<const wh::compose::graph>> chain{};
          chain.reserve(length);
          for (std::size_t offset = 0U; offset < length; ++offset) {
            const auto &entry = blocks.value()[(start + offset) % blocks.value().size()];
            chain.emplace_back(std::cref(entry.block_graph));
          }

          auto graph = make_block_chain(chain, mode);
          REQUIRE(graph.has_value());

          const int input = static_cast<int>(start + length);
          wh::core::run_context actual_context{};
          auto actual = invoke_value_sync(graph.value(), wh::core::any(input), actual_context);
          REQUIRE(actual.has_value());

          wh::compose::graph_value expected_payload = wh::core::any(input);
          for (std::size_t offset = 0U; offset < length; ++offset) {
            const auto &entry = blocks.value()[(start + offset) % blocks.value().size()];
            auto current = read_any<int>(std::move(expected_payload));
            REQUIRE(current.has_value());
            auto step = evaluate_case_block(entry, current.value());
            REQUIRE(step.has_value());
            expected_payload = std::move(step).value();
          }

          expect_same_block_output(std::move(actual).value(), std::move(expected_payload));
        }
      }
    }
  }
}

TEST_CASE("compose graph long mixed chain remains stable across repeated blocks",
          "[core][compose][graph][stress][chain]") {
  constexpr std::array modes{wh::compose::graph_runtime_mode::dag,
                             wh::compose::graph_runtime_mode::pregel};
  constexpr std::array block_counts{std::size_t{1U}, std::size_t{2U}, std::size_t{4U}};

  for (const auto mode : modes) {
    for (const auto block_count : block_counts) {
      DYNAMIC_SECTION((mode == wh::compose::graph_runtime_mode::pregel ? "pregel" : "dag")
                      << ":blocks=" << block_count) {
        wh::compose::graph_compile_options options{};
        options.mode = mode;
        options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
        options.fan_in_policy = wh::compose::graph_fan_in_policy::require_all_sources;
        options.max_parallel_nodes = 2U;
        options.max_parallel_per_node = 2U;
        wh::compose::graph graph{std::move(options)};

        const auto connect = [&graph](const std::string_view from,
                                      const std::string_view to) -> wh::core::result<void> {
          auto status = graph.add_edge(std::string{from}, std::string{to});
          if (status.has_error()) {
            return wh::core::result<void>::failure(status.error());
          }
          return {};
        };

        auto previous = std::string{wh::compose::graph_start_node_key};
        const auto append_node = [&graph, &connect,
                                  &previous](auto node) -> wh::core::result<void> {
          auto key = std::string{node.key()};
          auto added = add_test_node(graph, std::move(node));
          if (added.has_error()) {
            return wh::core::result<void>::failure(added.error());
          }
          auto linked = connect(previous, key);
          if (linked.has_error()) {
            return linked;
          }
          previous = std::move(key);
          return {};
        };

        for (std::size_t block = 0U; block < block_count; ++block) {
          const auto prefix = std::string{"block_"} + std::to_string(block);

          REQUIRE(append_node(wh::compose::make_lambda_node<wh::compose::node_contract::value,
                                                            wh::compose::node_contract::value,
                                                            wh::compose::node_exec_mode::async>(
                                  prefix + "_lambda_async",
                                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                                     const wh::compose::graph_call_scope &) {
                                    auto typed = read_any<int>(input);
                                    if (typed.has_error()) {
                                      return stdexec::just(
                                          wh::core::result<wh::compose::graph_value>::failure(
                                              typed.error()));
                                    }
                                    return stdexec::just(wh::core::result<wh::compose::graph_value>{
                                        wh::core::any(typed.value() + 20)});
                                  }))
                      .has_value());

          REQUIRE(append_node(wh::compose::make_lambda_node(
                                  prefix + "_model_pre",
                                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                                     const wh::compose::graph_call_scope &)
                                      -> wh::core::result<wh::compose::graph_value> {
                                    auto typed = read_any<int>(input);
                                    if (typed.has_error()) {
                                      return wh::core::result<wh::compose::graph_value>::failure(
                                          typed.error());
                                    }
                                    return wh::core::any(
                                        make_chat_request(std::to_string(typed.value())));
                                  }))
                      .has_value());

          REQUIRE(append_node(wh::compose::make_component_node<wh::core::component_kind::model,
                                                               wh::compose::node_contract::value,
                                                               wh::compose::node_contract::value>(
                                  prefix + "_model", wh::model::echo_chat_model{}))
                      .has_value());

          REQUIRE(append_node(wh::compose::make_lambda_node(
                                  prefix + "_model_post",
                                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                                     const wh::compose::graph_call_scope &)
                                      -> wh::core::result<wh::compose::graph_value> {
                                    auto typed = read_any<wh::model::chat_response>(input);
                                    if (typed.has_error()) {
                                      return wh::core::result<wh::compose::graph_value>::failure(
                                          typed.error());
                                    }
                                    auto value = read_message_text_int(typed.value().message);
                                    if (value.has_error()) {
                                      return wh::core::result<wh::compose::graph_value>::failure(
                                          value.error());
                                    }
                                    return wh::core::any(value.value() + 1);
                                  }))
                      .has_value());

          REQUIRE(append_node(wh::compose::make_lambda_node(
                                  prefix + "_prompt_pre",
                                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                                     const wh::compose::graph_call_scope &)
                                      -> wh::core::result<wh::compose::graph_value> {
                                    auto typed = read_any<int>(input);
                                    if (typed.has_error()) {
                                      return wh::core::result<wh::compose::graph_value>::failure(
                                          typed.error());
                                    }
                                    return wh::core::any(
                                        make_prompt_request(std::to_string(typed.value())));
                                  }))
                      .has_value());

          REQUIRE(
              append_node(wh::compose::make_component_node<wh::core::component_kind::prompt,
                                                           wh::compose::node_contract::value,
                                                           wh::compose::node_contract::value>(
                              prefix + "_prompt",
                              wh::prompt::simple_chat_template({wh::prompt::prompt_message_template{
                                  wh::schema::message_role::user, "{{name}}", "user"}})))
                  .has_value());

          REQUIRE(append_node(wh::compose::make_lambda_node(
                                  prefix + "_prompt_post",
                                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                                     const wh::compose::graph_call_scope &)
                                      -> wh::core::result<wh::compose::graph_value> {
                                    auto typed = read_any<std::vector<wh::schema::message>>(input);
                                    if (typed.has_error()) {
                                      return wh::core::result<wh::compose::graph_value>::failure(
                                          typed.error());
                                    }
                                    auto value = read_message_text_int(typed.value().front());
                                    if (value.has_error()) {
                                      return wh::core::result<wh::compose::graph_value>::failure(
                                          value.error());
                                    }
                                    return wh::core::any(value.value() + 2);
                                  }))
                      .has_value());

          REQUIRE(append_node(
                      wh::compose::make_lambda_node(
                          prefix + "_embedding_pre",
                          [](const wh::compose::graph_value &input, wh::core::run_context &,
                             const wh::compose::graph_call_scope &)
                              -> wh::core::result<wh::compose::graph_value> {
                            auto typed = read_any<int>(input);
                            if (typed.has_error()) {
                              return wh::core::result<wh::compose::graph_value>::failure(
                                  typed.error());
                            }
                            return wh::core::any(wh::embedding::embedding_request{
                                .inputs = std::vector<std::string>{std::to_string(typed.value())}});
                          }))
                      .has_value());

          REQUIRE(
              append_node(
                  wh::compose::make_component_node<wh::core::component_kind::embedding,
                                                   wh::compose::node_contract::value,
                                                   wh::compose::node_contract::value>(
                      prefix + "_embedding",
                      wh::embedding::embedding{sync_embedding_impl{
                          [](const wh::embedding::embedding_request &request)
                              -> wh::core::result<wh::embedding::embedding_response> {
                            auto value = parse_int_text(request.inputs.front());
                            if (value.has_error()) {
                              return wh::core::result<wh::embedding::embedding_response>::failure(
                                  value.error());
                            }
                            return wh::embedding::embedding_response{
                                std::vector<std::vector<double>>{
                                    {static_cast<double>(value.value())}}};
                          }}}))
                  .has_value());

          REQUIRE(append_node(wh::compose::make_lambda_node(
                                  prefix + "_embedding_post",
                                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                                     const wh::compose::graph_call_scope &)
                                      -> wh::core::result<wh::compose::graph_value> {
                                    auto typed = read_any<wh::embedding::embedding_response>(input);
                                    if (typed.has_error()) {
                                      return wh::core::result<wh::compose::graph_value>::failure(
                                          typed.error());
                                    }
                                    return wh::core::any(
                                        static_cast<int>(typed.value().front().front()) + 4);
                                  }))
                      .has_value());

          REQUIRE(append_node(wh::compose::make_lambda_node(
                                  prefix + "_retriever_pre",
                                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                                     const wh::compose::graph_call_scope &)
                                      -> wh::core::result<wh::compose::graph_value> {
                                    auto typed = read_any<int>(input);
                                    if (typed.has_error()) {
                                      return wh::core::result<wh::compose::graph_value>::failure(
                                          typed.error());
                                    }
                                    return wh::core::any(wh::retriever::retriever_request{
                                        .query = std::to_string(typed.value()), .index = "idx"});
                                  }))
                      .has_value());

          REQUIRE(append_node(wh::compose::make_component_node<wh::core::component_kind::retriever,
                                                               wh::compose::node_contract::value,
                                                               wh::compose::node_contract::value>(
                                  prefix + "_retriever",
                                  wh::retriever::retriever{sync_retriever_impl{
                                      [](const wh::retriever::retriever_request &request)
                                          -> wh::core::result<wh::retriever::retriever_response> {
                                        return wh::retriever::retriever_response{
                                            std::vector<wh::schema::document>{
                                                wh::schema::document{request.query}}};
                                      }}}))
                      .has_value());

          REQUIRE(append_node(wh::compose::make_lambda_node(
                                  prefix + "_retriever_post",
                                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                                     const wh::compose::graph_call_scope &)
                                      -> wh::core::result<wh::compose::graph_value> {
                                    auto typed = read_any<wh::retriever::retriever_response>(input);
                                    if (typed.has_error()) {
                                      return wh::core::result<wh::compose::graph_value>::failure(
                                          typed.error());
                                    }
                                    auto value = parse_int_text(typed.value().front().content());
                                    if (value.has_error()) {
                                      return wh::core::result<wh::compose::graph_value>::failure(
                                          value.error());
                                    }
                                    return wh::core::any(value.value() + 5);
                                  }))
                      .has_value());

          REQUIRE(
              append_node(
                  wh::compose::make_lambda_node(
                      prefix + "_indexer_pre",
                      [](const wh::compose::graph_value &input, wh::core::run_context &,
                         const wh::compose::graph_call_scope &)
                          -> wh::core::result<wh::compose::graph_value> {
                        auto typed = read_any<int>(input);
                        if (typed.has_error()) {
                          return wh::core::result<wh::compose::graph_value>::failure(typed.error());
                        }
                        return wh::core::any(wh::indexer::indexer_request{
                            .documents = {wh::schema::document{std::to_string(typed.value())}}});
                      }))
                  .has_value());

          REQUIRE(append_node(wh::compose::make_component_node<wh::core::component_kind::indexer,
                                                               wh::compose::node_contract::value,
                                                               wh::compose::node_contract::value>(
                                  prefix + "_indexer",
                                  wh::indexer::indexer{sync_indexer_batch_impl{
                                      [](const wh::indexer::indexer_request &request)
                                          -> wh::core::result<wh::indexer::indexer_response> {
                                        wh::indexer::indexer_response response{};
                                        response.document_ids.push_back(
                                            request.documents.front().content());
                                        response.success_count = 1U;
                                        return response;
                                      }}}))
                      .has_value());

          REQUIRE(append_node(wh::compose::make_lambda_node(
                                  prefix + "_indexer_post",
                                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                                     const wh::compose::graph_call_scope &)
                                      -> wh::core::result<wh::compose::graph_value> {
                                    auto typed = read_any<wh::indexer::indexer_response>(input);
                                    if (typed.has_error()) {
                                      return wh::core::result<wh::compose::graph_value>::failure(
                                          typed.error());
                                    }
                                    auto value = parse_int_text(typed.value().document_ids.front());
                                    if (value.has_error()) {
                                      return wh::core::result<wh::compose::graph_value>::failure(
                                          value.error());
                                    }
                                    return wh::core::any(value.value() + 6);
                                  }))
                      .has_value());

          REQUIRE(append_node(wh::compose::make_lambda_node(
                                  prefix + "_document_pre",
                                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                                     const wh::compose::graph_call_scope &)
                                      -> wh::core::result<wh::compose::graph_value> {
                                    auto typed = read_any<int>(input);
                                    if (typed.has_error()) {
                                      return wh::core::result<wh::compose::graph_value>::failure(
                                          typed.error());
                                    }
                                    return wh::core::any(
                                        make_document_request(std::to_string(typed.value())));
                                  }))
                      .has_value());

          REQUIRE(append_node(wh::compose::make_component_node<wh::core::component_kind::document,
                                                               wh::compose::node_contract::value,
                                                               wh::compose::node_contract::value>(
                                  prefix + "_document",
                                  wh::document::document{wh::document::document_processor{
                                      wh::document::parser::make_text_parser()}}))
                      .has_value());

          REQUIRE(append_node(wh::compose::make_lambda_node(
                                  prefix + "_document_post",
                                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                                     const wh::compose::graph_call_scope &)
                                      -> wh::core::result<wh::compose::graph_value> {
                                    auto typed = read_any<wh::document::document_batch>(input);
                                    if (typed.has_error()) {
                                      return wh::core::result<wh::compose::graph_value>::failure(
                                          typed.error());
                                    }
                                    auto value = parse_int_text(typed.value().front().content());
                                    if (value.has_error()) {
                                      return wh::core::result<wh::compose::graph_value>::failure(
                                          value.error());
                                    }
                                    return wh::core::any(value.value() + 7);
                                  }))
                      .has_value());

          REQUIRE(append_node(wh::compose::make_lambda_node(
                                  prefix + "_component_tool_pre",
                                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                                     const wh::compose::graph_call_scope &)
                                      -> wh::core::result<wh::compose::graph_value> {
                                    auto typed = read_any<int>(input);
                                    if (typed.has_error()) {
                                      return wh::core::result<wh::compose::graph_value>::failure(
                                          typed.error());
                                    }
                                    return wh::core::any(wh::tool::tool_request{
                                        .input_json = std::to_string(typed.value())});
                                  }))
                      .has_value());

          wh::schema::tool_schema_definition component_tool_schema{};
          component_tool_schema.name = prefix + "_component_tool";
          REQUIRE(append_node(
                      wh::compose::make_component_node<wh::core::component_kind::tool,
                                                       wh::compose::node_contract::value,
                                                       wh::compose::node_contract::value>(
                          prefix + "_component_tool",
                          wh::tool::tool{std::move(component_tool_schema),
                                         sync_tool_invoke_impl{[](const std::string_view input,
                                                                  const wh::tool::tool_options &)
                                                                   -> wh::tool::tool_invoke_result {
                                           return std::string{input};
                                         }}}))
                      .has_value());

          REQUIRE(append_node(wh::compose::make_lambda_node(
                                  prefix + "_component_tool_post",
                                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                                     const wh::compose::graph_call_scope &)
                                      -> wh::core::result<wh::compose::graph_value> {
                                    auto typed = read_any<std::string>(input);
                                    if (typed.has_error()) {
                                      return wh::core::result<wh::compose::graph_value>::failure(
                                          typed.error());
                                    }
                                    auto value = parse_int_text(typed.value());
                                    if (value.has_error()) {
                                      return wh::core::result<wh::compose::graph_value>::failure(
                                          value.error());
                                    }
                                    return wh::core::any(value.value() + 8);
                                  }))
                      .has_value());

          REQUIRE(append_node(wh::compose::make_lambda_node(
                                  prefix + "_tools_pre",
                                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                                     const wh::compose::graph_call_scope &)
                                      -> wh::core::result<wh::compose::graph_value> {
                                    auto typed = read_any<int>(input);
                                    if (typed.has_error()) {
                                      return wh::core::result<wh::compose::graph_value>::failure(
                                          typed.error());
                                    }
                                    return wh::core::any(make_tool_batch({wh::compose::tool_call{
                                        .call_id = "call",
                                        .tool_name = "echo",
                                        .arguments = std::to_string(typed.value())}}));
                                  }))
                      .has_value());

          wh::compose::tool_registry tools{};
          tools.insert_or_assign(
              "echo",
              wh::compose::tool_entry{
                  .async_invoke = [](wh::compose::tool_call call,
                                     wh::tool::call_scope) -> wh::compose::tools_invoke_sender {
                    return stdexec::just(
                        wh::core::result<wh::compose::graph_value>{wh::core::any(call.arguments)});
                  }});
          REQUIRE(append_node(wh::compose::make_tools_node<wh::compose::node_contract::value,
                                                           wh::compose::node_contract::value,
                                                           wh::compose::node_exec_mode::async>(
                                  prefix + "_tools", std::move(tools)))
                      .has_value());

          REQUIRE(
              append_node(
                  wh::compose::make_lambda_node(
                      prefix + "_tools_post",
                      [](const wh::compose::graph_value &input, wh::core::run_context &,
                         const wh::compose::graph_call_scope &)
                          -> wh::core::result<wh::compose::graph_value> {
                        auto results = collect_tool_results(input);
                        if (results.has_error()) {
                          return wh::core::result<wh::compose::graph_value>::failure(
                              results.error());
                        }
                        auto typed = read_any<std::string>(results.value().get().front().value);
                        if (typed.has_error()) {
                          return wh::core::result<wh::compose::graph_value>::failure(typed.error());
                        }
                        auto value = parse_int_text(typed.value());
                        if (value.has_error()) {
                          return wh::core::result<wh::compose::graph_value>::failure(value.error());
                        }
                        return wh::core::any(value.value() + 9);
                      }))
                  .has_value());

          REQUIRE(append_node(wh::compose::make_passthrough_node<wh::compose::node_contract::value>(
                                  prefix + "_pass_value"))
                      .has_value());

          auto child = make_value_subgraph(10);
          REQUIRE(child.has_value());
          REQUIRE(append_node(wh::compose::make_subgraph_node(prefix + "_subgraph",
                                                              std::move(child).value()))
                      .has_value());

          REQUIRE(append_node(
                      wh::compose::make_lambda_node<wh::compose::node_contract::value,
                                                    wh::compose::node_contract::stream>(
                          prefix + "_value_to_stream",
                          [](const wh::compose::graph_value &input, wh::core::run_context &,
                             const wh::compose::graph_call_scope &)
                              -> wh::core::result<wh::compose::graph_stream_reader> {
                            auto typed = read_any<int>(input);
                            if (typed.has_error()) {
                              return wh::core::result<wh::compose::graph_stream_reader>::failure(
                                  typed.error());
                            }
                            return make_int_graph_stream({typed.value(), typed.value() + 1});
                          }))
                      .has_value());

          REQUIRE(
              append_node(wh::compose::make_component_node<
                              wh::core::component_kind::custom, wh::compose::node_contract::stream,
                              wh::compose::node_contract::stream, wh::compose::graph_stream_reader,
                              wh::compose::graph_stream_reader>(prefix + "_stream_shift",
                                                                stream_shift_component{1}))
                  .has_value());

          REQUIRE(
              append_node(wh::compose::make_passthrough_node<wh::compose::node_contract::stream>(
                              prefix + "_pass_stream"))
                  .has_value());

          REQUIRE(append_node(wh::compose::make_lambda_node<wh::compose::node_contract::stream,
                                                            wh::compose::node_contract::value>(
                                  prefix + "_stream_sum",
                                  [](wh::compose::graph_stream_reader input,
                                     wh::core::run_context &, const wh::compose::graph_call_scope &)
                                      -> wh::core::result<wh::compose::graph_value> {
                                    auto summed = sum_int_stream(std::move(input));
                                    if (summed.has_error()) {
                                      return wh::core::result<wh::compose::graph_value>::failure(
                                          summed.error());
                                    }
                                    return wh::core::any(summed.value());
                                  }))
                      .has_value());
        }

        REQUIRE(connect(previous, wh::compose::graph_end_node_key).has_value());
        REQUIRE(graph.compile().has_value());

        for (int iteration = 0; iteration < 8; ++iteration) {
          wh::core::run_context context{};
          auto output = invoke_value_sync(graph, wh::core::any(iteration), context);
          REQUIRE(output.has_value());
          auto typed = read_any<int>(output.value());
          REQUIRE(typed.has_value());
          int expected = iteration;
          for (std::size_t block = 0U; block < block_count; ++block) {
            expected = 2 * expected + 147;
          }
          REQUIRE(typed.value() == expected);
        }
      }
    }
  }
}

TEST_CASE("compose graph lowering shape matrix remains stable", "[core][compose][graph][stress]") {
  constexpr std::array modes{wh::compose::graph_runtime_mode::dag,
                             wh::compose::graph_runtime_mode::pregel};
  constexpr std::array parallels{std::size_t{1U}, std::size_t{2U}};

  for (const auto source : all_source_shapes()) {
    for (const auto target : all_target_shapes()) {
      for (const auto style : all_lowering_styles()) {
        for (const auto mode : modes) {
          for (const auto parallel : parallels) {
            DYNAMIC_SECTION(shape_name(source)
                            << "->" << shape_name(target) << ":style=" << style_name(style)
                            << ":mode="
                            << (mode == wh::compose::graph_runtime_mode::pregel ? "pregel" : "dag")
                            << ":parallel=" << parallel) {
              auto graph = build_lowering_graph(source, target, style, mode, parallel);
              REQUIRE(graph.has_value());

              for (int iteration = 0; iteration < 16; ++iteration) {
                wh::core::run_context context{};
                auto output = invoke_value_sync(graph.value(), wh::core::any(iteration), context);
                REQUIRE(output.has_value());
                auto typed = read_any<int>(output.value());
                REQUIRE(typed.has_value());
                REQUIRE(typed.value() == expected_lowering_output(source, target, iteration));
              }
            }
          }
        }
      }
    }
  }
}

TEST_CASE("compose graph partial merged readers keep late lanes stable",
          "[core][compose][graph][stress]") {
  constexpr std::array modes{wh::compose::graph_runtime_mode::dag,
                             wh::compose::graph_runtime_mode::pregel};

  for (const auto mode : modes) {
    DYNAMIC_SECTION((mode == wh::compose::graph_runtime_mode::pregel ? "pregel" : "dag")) {
      exec::static_thread_pool pool{4U};
      wh::compose::graph_compile_options options{};
      options.mode = mode;
      options.trigger_mode = wh::compose::graph_trigger_mode::any_predecessor;
      options.fan_in_policy = wh::compose::graph_fan_in_policy::allow_partial;
      options.max_parallel_nodes = 1U;
      options.max_parallel_per_node = 4U;
      wh::compose::graph graph{std::move(options)};

      REQUIRE(graph
                  .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream,
                              wh::compose::node_exec_mode::async>(
                      "fast",
                      [](const wh::compose::graph_value &input, wh::core::run_context &,
                         const wh::compose::graph_call_scope &) {
                        const auto *typed = wh::core::any_cast<int>(&input);
                        return graph_stream_result_sender{wh::core::detail::normalize_result_sender<
                            wh::core::result<wh::compose::graph_stream_reader>>(
                            stdexec::just(wh::core::result<wh::compose::graph_stream_reader>{
                                make_int_graph_stream({*typed + 1, *typed + 2}).value()}))};
                      })
                  .has_value());
      REQUIRE(graph
                  .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream,
                              wh::compose::node_exec_mode::async>(
                      "slow",
                      [scheduler = pool.get_scheduler()](const wh::compose::graph_value &input,
                                                         wh::core::run_context &,
                                                         const wh::compose::graph_call_scope &) {
                        const auto *typed = wh::core::any_cast<int>(&input);
                        return graph_stream_result_sender{wh::core::detail::normalize_result_sender<
                            wh::core::result<wh::compose::graph_stream_reader>>(stdexec::starts_on(
                            scheduler,
                            stdexec::just(*typed) |
                                stdexec::then(
                                    [](int value)
                                        -> wh::core::result<wh::compose::graph_stream_reader> {
                                      std::this_thread::sleep_for(std::chrono::milliseconds{5});
                                      return make_int_graph_stream({value + 10, value + 11});
                                    })))};
                      })
                  .has_value());
      REQUIRE(graph
                  .add_lambda<wh::compose::node_contract::stream, wh::compose::node_contract::value,
                              wh::compose::node_exec_mode::async>(
                      "left",
                      [scheduler = pool.get_scheduler()](wh::compose::graph_stream_reader input,
                                                         wh::core::run_context &,
                                                         const wh::compose::graph_call_scope &) {
                        return stdexec::starts_on(
                            scheduler,
                            stdexec::just(std::move(input)) |
                                stdexec::then([](wh::compose::graph_stream_reader lane_input)
                                                  -> wh::core::result<wh::compose::graph_value> {
                                  auto observed = observe_stream(std::move(lane_input));
                                  if (observed.has_error()) {
                                    return wh::core::result<wh::compose::graph_value>::failure(
                                        observed.error());
                                  }
                                  wh::compose::graph_value_map output{};
                                  output.emplace("sum", wh::core::any(observed.value().sum));
                                  output.emplace("sources",
                                                 wh::core::any(observed.value().source_count));
                                  return wh::core::any(std::move(output));
                                }));
                      })
                  .has_value());
      REQUIRE(graph.add_entry_edge("fast").has_value());
      REQUIRE(graph.add_entry_edge("slow").has_value());
      REQUIRE(graph.add_edge("fast", "left").has_value());
      REQUIRE(graph.add_edge("slow", "left").has_value());
      REQUIRE(graph.add_exit_edge("left").has_value());
      REQUIRE(graph.compile().has_value());

      for (int iteration = 0; iteration < 16; ++iteration) {
        wh::core::run_context context{};
        auto output = invoke_value_sync(graph, wh::core::any(iteration), context);
        INFO("mode=" << (mode == wh::compose::graph_runtime_mode::dag ? "dag" : "pregel"));
        INFO("iteration=" << iteration);
        if (!output.has_value()) {
          INFO(output.error().message().c_str());
          INFO(output.error().value());
        }
        REQUIRE(output.has_value());
        auto result = read_any<wh::compose::graph_value_map>(output.value());
        REQUIRE(result.has_value());
        REQUIRE(read_named_int(result.value(), "sum").value() ==
                base_sum(source_shape::multi_stream, iteration));
        REQUIRE(read_named_int(result.value(), "sources").value() == 2);
      }
    }
  }
}

TEST_CASE("compose graph partial aggregation follows runtime barrier semantics",
          "[core][compose][graph][stress]") {
  constexpr std::array modes{wh::compose::graph_runtime_mode::dag,
                             wh::compose::graph_runtime_mode::pregel};

  for (const auto mode : modes) {
    DYNAMIC_SECTION((mode == wh::compose::graph_runtime_mode::pregel ? "pregel" : "dag")) {
      exec::static_thread_pool pool{4U};
      wh::compose::graph_compile_options options{};
      options.mode = mode;
      options.trigger_mode = wh::compose::graph_trigger_mode::any_predecessor;
      options.fan_in_policy = wh::compose::graph_fan_in_policy::allow_partial;
      options.max_parallel_nodes = 4U;
      options.max_parallel_per_node = 4U;
      wh::compose::graph graph{std::move(options)};

      REQUIRE(graph
                  .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream,
                              wh::compose::node_exec_mode::async>(
                      "fast",
                      [](const wh::compose::graph_value &input, wh::core::run_context &,
                         const wh::compose::graph_call_scope &) {
                        const auto *typed = wh::core::any_cast<int>(&input);
                        return graph_stream_result_sender{wh::core::detail::normalize_result_sender<
                            wh::core::result<wh::compose::graph_stream_reader>>(
                            stdexec::just(wh::core::result<wh::compose::graph_stream_reader>{
                                make_int_graph_stream({*typed + 1, *typed + 2}).value()}))};
                      })
                  .has_value());
      REQUIRE(graph
                  .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream,
                              wh::compose::node_exec_mode::async>(
                      "slow",
                      [scheduler = pool.get_scheduler()](const wh::compose::graph_value &input,
                                                         wh::core::run_context &,
                                                         const wh::compose::graph_call_scope &) {
                        const auto *typed = wh::core::any_cast<int>(&input);
                        return graph_stream_result_sender{wh::core::detail::normalize_result_sender<
                            wh::core::result<wh::compose::graph_stream_reader>>(stdexec::starts_on(
                            scheduler,
                            stdexec::just(*typed) |
                                stdexec::then(
                                    [](int value)
                                        -> wh::core::result<wh::compose::graph_stream_reader> {
                                      std::this_thread::sleep_for(std::chrono::milliseconds{5});
                                      return make_int_graph_stream({value + 10, value + 11});
                                    })))};
                      })
                  .has_value());

      REQUIRE(graph
                  .add_lambda<wh::compose::node_contract::stream, wh::compose::node_contract::value,
                              wh::compose::node_exec_mode::async>(
                      "left",
                      [scheduler = pool.get_scheduler()](wh::compose::graph_stream_reader input,
                                                         wh::core::run_context &,
                                                         const wh::compose::graph_call_scope &) {
                        return stdexec::starts_on(
                            scheduler,
                            stdexec::just(std::move(input)) |
                                stdexec::then([](wh::compose::graph_stream_reader lane_input)
                                                  -> wh::core::result<wh::compose::graph_value> {
                                  auto observed = observe_stream(std::move(lane_input));
                                  if (observed.has_error()) {
                                    return wh::core::result<wh::compose::graph_value>::failure(
                                        observed.error());
                                  }
                                  wh::compose::graph_value_map output{};
                                  output.emplace("sum", wh::core::any(observed.value().sum));
                                  output.emplace("sources",
                                                 wh::core::any(observed.value().source_count));
                                  return wh::core::any(std::move(output));
                                }));
                      })
                  .has_value());
      REQUIRE(graph
                  .add_lambda<wh::compose::node_contract::stream, wh::compose::node_contract::value,
                              wh::compose::node_exec_mode::async>(
                      "right",
                      [scheduler = pool.get_scheduler()](wh::compose::graph_stream_reader input,
                                                         wh::core::run_context &,
                                                         const wh::compose::graph_call_scope &) {
                        return stdexec::starts_on(
                            scheduler,
                            stdexec::just(std::move(input)) |
                                stdexec::then([](wh::compose::graph_stream_reader lane_input)
                                                  -> wh::core::result<wh::compose::graph_value> {
                                  auto observed = observe_stream(std::move(lane_input));
                                  if (observed.has_error()) {
                                    return wh::core::result<wh::compose::graph_value>::failure(
                                        observed.error());
                                  }
                                  std::this_thread::sleep_for(std::chrono::milliseconds{20});
                                  wh::compose::graph_value_map output{};
                                  output.emplace("sum", wh::core::any(observed.value().sum));
                                  output.emplace("sources",
                                                 wh::core::any(observed.value().source_count));
                                  return wh::core::any(std::move(output));
                                }));
                      })
                  .has_value());

      REQUIRE(graph
                  .add_lambda(
                      "join",
                      [](const wh::compose::graph_value &input, wh::core::run_context &,
                         const wh::compose::graph_call_scope &)
                          -> wh::core::result<wh::compose::graph_value> {
                        auto merged = read_any<wh::compose::graph_value_map>(input);
                        if (merged.has_error()) {
                          return wh::core::result<wh::compose::graph_value>::failure(
                              merged.error());
                        }
                        wh::compose::graph_value_map output{};
                        output.emplace("contributors",
                                       wh::core::any(static_cast<int>(merged.value().size())));
                        if (merged.value().contains("left")) {
                          output.emplace(
                              "left_sum",
                              wh::core::any(read_named_int(read_any<wh::compose::graph_value_map>(
                                                               merged.value().at("left"))
                                                               .value(),
                                                           "sum")
                                                .value()));
                        }
                        if (merged.value().contains("right")) {
                          output.emplace(
                              "right_sum",
                              wh::core::any(read_named_int(read_any<wh::compose::graph_value_map>(
                                                               merged.value().at("right"))
                                                               .value(),
                                                           "sum")
                                                .value()));
                        }
                        return wh::core::any(std::move(output));
                      })
                  .has_value());

      REQUIRE(graph.add_entry_edge("fast").has_value());
      REQUIRE(graph.add_entry_edge("slow").has_value());
      REQUIRE(graph.add_edge("fast", "left").has_value());
      REQUIRE(graph.add_edge("slow", "left").has_value());
      REQUIRE(graph.add_edge("fast", "right").has_value());
      REQUIRE(graph.add_edge("slow", "right").has_value());
      REQUIRE(graph.add_edge("left", "join").has_value());
      REQUIRE(graph.add_edge("right", "join").has_value());
      REQUIRE(graph.add_exit_edge("join").has_value());
      REQUIRE(graph.compile().has_value());

      for (int iteration = 0; iteration < 16; ++iteration) {
        wh::core::run_context context{};
        wh::compose::graph_call_options call_options{};
        call_options.record_transition_log = true;
        auto status = invoke_graph_sync(graph, wh::core::any(iteration), context, call_options, {});
        if (!status.has_value()) {
          const std::string mode_text =
              mode == wh::compose::graph_runtime_mode::dag ? "dag" : "pregel";
          std::string diag = "mode=" + mode_text + " iteration=" + std::to_string(iteration) +
                             " error=" + status.error().message() + "#" +
                             std::to_string(status.error().value());

          if (!status.value().report.completed_node_keys.empty()) {
            std::string completed_text{};
            for (const auto &node_key : status.value().report.completed_node_keys) {
              if (!completed_text.empty()) {
                completed_text += ",";
              }
              completed_text += node_key;
            }
            diag += " completed=[" + completed_text + "]";
          }

          if (!status.value().report.transition_log.empty()) {
            std::string tail{};
            const auto &entries = status.value().report.transition_log;
            const auto begin = entries.size() > 6U ? entries.size() - 6U : 0U;
            for (std::size_t index = begin; index < entries.size(); ++index) {
              const auto &event = entries[index];
              if (!tail.empty()) {
                tail += " | ";
              }
              tail += event.cause.node_key;
              tail += '#';
              tail += std::to_string(static_cast<int>(event.kind));
              tail += '@';
              tail += std::to_string(event.cause.step);
            }
            diag += " tail=[" + tail + "]";
          }
          FAIL(diag);
        }
        REQUIRE(status.value().output_status.has_value());
        auto result = read_any<wh::compose::graph_value_map>(status.value().output_status.value());
        REQUIRE(result.has_value());
        if (mode == wh::compose::graph_runtime_mode::dag) {
          REQUIRE(read_named_int(result.value(), "contributors").value() == 1);
          REQUIRE(result.value().contains("left_sum"));
          REQUIRE_FALSE(result.value().contains("right_sum"));
          REQUIRE(read_named_int(result.value(), "left_sum").value() ==
                  base_sum(source_shape::multi_stream, iteration));
          continue;
        }

        REQUIRE(read_named_int(result.value(), "contributors").value() == 2);
        REQUIRE(result.value().contains("left_sum"));
        REQUIRE(result.value().contains("right_sum"));
        REQUIRE(read_named_int(result.value(), "left_sum").value() ==
                base_sum(source_shape::multi_stream, iteration));
        REQUIRE(read_named_int(result.value(), "right_sum").value() ==
                base_sum(source_shape::multi_stream, iteration));
      }
    }
  }
}

TEST_CASE("compose graph allow-no-data stream fallback remains stable",
          "[core][compose][graph][stress]") {
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

  for (int iteration = 0; iteration < 16; ++iteration) {
    wh::core::run_context context{};
    auto output = invoke_value_sync(graph, wh::core::any(iteration), context);
    REQUIRE(output.has_value());
    auto typed = read_any<int>(output.value());
    REQUIRE(typed.has_value());
    REQUIRE(typed.value() == 0);
  }
}

enum class pace_case : std::uint8_t {
  fast_fast = 0U,
  slow_source,
  slow_sink,
  slow_both,
};

[[nodiscard]] constexpr auto all_pace_cases() noexcept -> std::array<pace_case, 4> {
  return {pace_case::fast_fast, pace_case::slow_source, pace_case::slow_sink, pace_case::slow_both};
}

[[nodiscard]] constexpr auto pace_name(const pace_case value) noexcept -> std::string_view {
  switch (value) {
  case pace_case::fast_fast:
    return "fast_fast";
  case pace_case::slow_source:
    return "slow_source";
  case pace_case::slow_sink:
    return "slow_sink";
  case pace_case::slow_both:
    return "slow_both";
  }
  return "unknown";
}

[[nodiscard]] constexpr auto source_delay(const pace_case value) noexcept
    -> std::chrono::milliseconds {
  switch (value) {
  case pace_case::fast_fast:
  case pace_case::slow_sink:
    return std::chrono::milliseconds{0};
  case pace_case::slow_source:
  case pace_case::slow_both:
    return std::chrono::milliseconds{3};
  }
  return std::chrono::milliseconds{0};
}

[[nodiscard]] constexpr auto right_sink_delay(const pace_case value) noexcept
    -> std::chrono::milliseconds {
  switch (value) {
  case pace_case::fast_fast:
  case pace_case::slow_source:
    return std::chrono::milliseconds{0};
  case pace_case::slow_sink:
  case pace_case::slow_both:
    return std::chrono::milliseconds{5};
  }
  return std::chrono::milliseconds{0};
}

[[nodiscard]] auto build_paced_graph(exec::static_thread_pool::scheduler scheduler,
                                     const wh::compose::graph_runtime_mode mode,
                                     const pace_case pace, const std::size_t parallel)
    -> wh::core::result<wh::compose::graph> {
  wh::compose::graph_compile_options options{};
  options.mode = mode;
  options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
  options.fan_in_policy = wh::compose::graph_fan_in_policy::require_all_sources;
  options.max_parallel_nodes = parallel;
  options.max_parallel_per_node = parallel;
  wh::compose::graph graph{std::move(options)};

  const auto source_wait = source_delay(pace);
  REQUIRE(
      graph
          .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream,
                      wh::compose::node_exec_mode::async>(
              "source",
              [scheduler, source_wait](const wh::compose::graph_value &input,
                                       wh::core::run_context &,
                                       const wh::compose::graph_call_scope &) {
                const auto *typed = wh::core::any_cast<int>(&input);
                return graph_stream_result_sender{wh::core::detail::normalize_result_sender<
                    wh::core::result<wh::compose::graph_stream_reader>>(stdexec::starts_on(
                    scheduler,
                    stdexec::just(*typed) |
                        stdexec::then([source_wait](int value)
                                          -> wh::core::result<wh::compose::graph_stream_reader> {
                          if (source_wait != std::chrono::milliseconds{0}) {
                            std::this_thread::sleep_for(source_wait);
                          }
                          return make_int_graph_stream({value + 1, value + 2, value + 3});
                        })))};
              })
          .has_value());

  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::stream, wh::compose::node_contract::value,
                          wh::compose::node_exec_mode::async>(
                  "left",
                  [scheduler](wh::compose::graph_stream_reader input, wh::core::run_context &,
                              const wh::compose::graph_call_scope &) {
                    return stdexec::starts_on(
                        scheduler,
                        stdexec::just(std::move(input)) |
                            stdexec::then([](wh::compose::graph_stream_reader reader)
                                              -> wh::core::result<wh::compose::graph_value> {
                              auto values = collect_int_graph_stream(std::move(reader));
                              if (values.has_error()) {
                                return wh::core::result<wh::compose::graph_value>::failure(
                                    values.error());
                              }
                              return wh::core::any(sum_ints(values.value()) + 10);
                            }));
                  })
              .has_value());

  const auto right_wait = right_sink_delay(pace);
  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::stream, wh::compose::node_contract::value,
                          wh::compose::node_exec_mode::async>(
                  "right",
                  [scheduler, right_wait](wh::compose::graph_stream_reader input,
                                          wh::core::run_context &,
                                          const wh::compose::graph_call_scope &) {
                    return stdexec::starts_on(
                        scheduler,
                        stdexec::just(std::move(input)) |
                            stdexec::then([right_wait](wh::compose::graph_stream_reader reader)
                                              -> wh::core::result<wh::compose::graph_value> {
                              auto values = collect_int_graph_stream(std::move(reader));
                              if (values.has_error()) {
                                return wh::core::result<wh::compose::graph_value>::failure(
                                    values.error());
                              }
                              if (right_wait != std::chrono::milliseconds{0}) {
                                std::this_thread::sleep_for(right_wait);
                              }
                              return wh::core::any(sum_ints(values.value()) + 20);
                            }));
                  })
              .has_value());

  REQUIRE(graph
              .add_lambda(
                  "join",
                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    auto merged = read_any<wh::compose::graph_value_map>(input);
                    if (merged.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(merged.error());
                    }
                    auto left = read_named_int(merged.value(), "left");
                    auto right = read_named_int(merged.value(), "right");
                    if (left.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(left.error());
                    }
                    if (right.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(right.error());
                    }
                    return wh::core::any(left.value() + right.value());
                  })
              .has_value());

  REQUIRE(graph.add_entry_edge("source").has_value());
  REQUIRE(graph.add_edge("source", "left").has_value());
  REQUIRE(graph.add_edge("source", "right").has_value());
  REQUIRE(graph.add_edge("left", "join").has_value());
  REQUIRE(graph.add_edge("right", "join").has_value());
  REQUIRE(graph.add_exit_edge("join").has_value());
  REQUIRE(graph.compile().has_value());
  return graph;
}

[[nodiscard]] auto build_loop_region_graph(const wh::compose::graph_runtime_mode mode)
    -> wh::core::result<wh::compose::graph> {
  wh::compose::graph_compile_options options{};
  options.mode = mode;
  options.max_steps = 32U;
  wh::compose::graph graph{std::move(options)};

  REQUIRE(graph
              .add_lambda("entry",
                          [](const wh::compose::graph_value &input, wh::core::run_context &,
                             const wh::compose::graph_call_scope &)
                              -> wh::core::result<wh::compose::graph_value> {
                            auto typed = read_any<int>(input);
                            if (typed.has_error()) {
                              return wh::core::result<wh::compose::graph_value>::failure(
                                  typed.error());
                            }
                            return wh::core::any(typed.value() + 1);
                          })
              .has_value());
  REQUIRE(graph
              .add_lambda("loop_a",
                          [](const wh::compose::graph_value &input, wh::core::run_context &,
                             const wh::compose::graph_call_scope &)
                              -> wh::core::result<wh::compose::graph_value> {
                            auto typed = sum_graph_value(input);
                            if (typed.has_error()) {
                              return wh::core::result<wh::compose::graph_value>::failure(
                                  typed.error());
                            }
                            return wh::core::any(typed.value() + 2);
                          })
              .has_value());
  REQUIRE(graph
              .add_lambda("loop_b",
                          [](const wh::compose::graph_value &input, wh::core::run_context &,
                             const wh::compose::graph_call_scope &)
                              -> wh::core::result<wh::compose::graph_value> {
                            auto typed = read_any<int>(input);
                            if (typed.has_error()) {
                              return wh::core::result<wh::compose::graph_value>::failure(
                                  typed.error());
                            }
                            return wh::core::any(typed.value() + 3);
                          })
              .has_value());
  REQUIRE(graph
              .add_lambda("tail",
                          [](const wh::compose::graph_value &input, wh::core::run_context &,
                             const wh::compose::graph_call_scope &)
                              -> wh::core::result<wh::compose::graph_value> {
                            auto typed = read_any<int>(input);
                            if (typed.has_error()) {
                              return wh::core::result<wh::compose::graph_value>::failure(
                                  typed.error());
                            }
                            return wh::core::any(typed.value() + 100);
                          })
              .has_value());

  REQUIRE(graph.add_entry_edge("entry").has_value());
  REQUIRE(graph.add_edge("entry", "loop_a").has_value());
  REQUIRE(graph.add_edge("loop_a", "loop_b").has_value());
  REQUIRE(graph.add_edge("loop_b", "loop_a").has_value());
  REQUIRE(graph.add_edge("loop_b", "tail").has_value());
  REQUIRE(graph.add_exit_edge("tail").has_value());

  auto compiled = graph.compile();
  if (compiled.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(compiled.error());
  }
  return graph;
}

[[nodiscard]] auto build_concurrent_graph(exec::static_thread_pool::scheduler scheduler,
                                          const wh::compose::graph_runtime_mode mode)
    -> wh::core::result<wh::compose::graph> {
  wh::compose::graph_compile_options options{};
  options.mode = mode;
  options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
  options.fan_in_policy = wh::compose::graph_fan_in_policy::require_all_sources;
  options.max_parallel_nodes = 2U;
  options.max_parallel_per_node = 2U;
  wh::compose::graph graph{std::move(options)};

  REQUIRE(
      graph
          .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream,
                      wh::compose::node_exec_mode::async>(
              "source",
              [scheduler](const wh::compose::graph_value &input, wh::core::run_context &,
                          const wh::compose::graph_call_scope &) {
                const auto *typed = wh::core::any_cast<int>(&input);
                return graph_stream_result_sender{wh::core::detail::normalize_result_sender<
                    wh::core::result<wh::compose::graph_stream_reader>>(stdexec::starts_on(
                    scheduler,
                    stdexec::just(*typed) |
                        stdexec::then(
                            [](int value) -> wh::core::result<wh::compose::graph_stream_reader> {
                              std::this_thread::sleep_for(std::chrono::milliseconds{2});
                              return make_int_graph_stream({value + 1, value + 2, value + 3});
                            })))};
              })
          .has_value());
  REQUIRE(add_stream_sink(graph, "left", 10).has_value());
  REQUIRE(add_stream_sink(graph, "right", 20).has_value());
  REQUIRE(graph
              .add_lambda(
                  "join",
                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    auto merged = read_any<wh::compose::graph_value_map>(input);
                    if (merged.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(merged.error());
                    }
                    auto left = read_named_int(merged.value(), "left");
                    auto right = read_named_int(merged.value(), "right");
                    if (left.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(left.error());
                    }
                    if (right.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(right.error());
                    }
                    return wh::core::any(left.value() + right.value());
                  })
              .has_value());

  REQUIRE(graph.add_entry_edge("source").has_value());
  REQUIRE(graph.add_edge("source", "left").has_value());
  REQUIRE(graph.add_edge("source", "right").has_value());
  REQUIRE(graph.add_edge("left", "join").has_value());
  REQUIRE(graph.add_edge("right", "join").has_value());
  REQUIRE(graph.add_exit_edge("join").has_value());
  auto compiled = graph.compile();
  if (compiled.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(compiled.error());
  }
  return graph;
}

template <typename expected_fn_t>
auto require_concurrent_graph_invoke_stable(const wh::compose::graph &graph,
                                            expected_fn_t expected_fn, const int worker_count,
                                            const int rounds_per_worker) -> void {
  std::atomic<int> failures{0};
  std::mutex error_mutex{};
  std::string error_text{};
  std::vector<std::thread> workers{};
  workers.reserve(static_cast<std::size_t>(worker_count));

  for (int worker = 0; worker < worker_count; ++worker) {
    workers.emplace_back([&, worker] {
      for (int round = 0; round < rounds_per_worker; ++round) {
        const int input = worker * 100 + round;
        wh::core::run_context context{};
        auto output = invoke_value_sync(graph, wh::core::any(input), context);
        if (output.has_error()) {
          failures.fetch_add(1);
          std::lock_guard lock{error_mutex};
          if (error_text.empty()) {
            error_text = output.error().message();
          }
          return;
        }
        auto typed = read_any<int>(output.value());
        const auto expected = expected_fn(input);
        if (typed.has_error() || typed.value() != expected) {
          failures.fetch_add(1);
          std::lock_guard lock{error_mutex};
          if (error_text.empty()) {
            error_text = typed.has_error()
                             ? typed.error().message()
                             : "unexpected output:" + std::to_string(input) + ":" +
                                   std::to_string(typed.value()) + ":" + std::to_string(expected);
          }
          return;
        }
      }
    });
  }

  for (auto &worker : workers) {
    worker.join();
  }

  REQUIRE(failures.load() == 0);
  REQUIRE(error_text.empty());
}

TEST_CASE("compose graph concurrent invoke stress remains stable",
          "[core][compose][graph][stress]") {
  constexpr std::array modes{wh::compose::graph_runtime_mode::dag,
                             wh::compose::graph_runtime_mode::pregel};

  for (const auto mode : modes) {
    DYNAMIC_SECTION((mode == wh::compose::graph_runtime_mode::pregel ? "pregel" : "dag")) {
      exec::static_thread_pool pool{4U};
      auto graph = build_concurrent_graph(pool.get_scheduler(), mode);
      REQUIRE(graph.has_value());
      require_concurrent_graph_invoke_stable(
          graph.value(), [](const int input) { return 6 * input + 42; }, 6, 16);
    }
  }
}

TEST_CASE("compose graph stream fan-out lowering concurrent invoke remains stable",
          "[core][compose][graph][stress][lowering][stream]") {
  constexpr std::array modes{wh::compose::graph_runtime_mode::dag,
                             wh::compose::graph_runtime_mode::pregel};

  for (const auto mode : modes) {
    DYNAMIC_SECTION((mode == wh::compose::graph_runtime_mode::pregel ? "pregel" : "dag")) {
      auto graph = build_lowering_graph(source_shape::single_stream, target_shape::multi_value,
                                        lowering_style::builtin, mode, 4U);
      REQUIRE(graph.has_value());

      require_concurrent_graph_invoke_stable(
          graph.value(),
          [](const int input) {
            return expected_lowering_output(source_shape::single_stream, target_shape::multi_value,
                                            input);
          },
          6, 16);
    }
  }
}

TEST_CASE("compose graph stream fan-in lowering concurrent invoke remains stable",
          "[core][compose][graph][stress][lowering][stream]") {
  constexpr std::array modes{wh::compose::graph_runtime_mode::dag,
                             wh::compose::graph_runtime_mode::pregel};

  for (const auto mode : modes) {
    DYNAMIC_SECTION((mode == wh::compose::graph_runtime_mode::pregel ? "pregel" : "dag")) {
      auto graph = build_lowering_graph(source_shape::multi_stream, target_shape::single_value,
                                        lowering_style::builtin, mode, 4U);
      REQUIRE(graph.has_value());

      require_concurrent_graph_invoke_stable(
          graph.value(),
          [](const int input) {
            return expected_lowering_output(source_shape::multi_stream, target_shape::single_value,
                                            input);
          },
          6, 16);
    }
  }
}

TEST_CASE("compose graph synchronized async value fan-in remains stable across "
          "sequential invokes",
          "[core][compose][graph][value][stress][concurrency][fanin][synchronized]"
          "[sequential]") {
  constexpr std::array dispatch_policies{
      wh::compose::graph_dispatch_policy::same_wave,
      wh::compose::graph_dispatch_policy::next_wave,
  };

  for (const auto dispatch_policy : dispatch_policies) {
    DYNAMIC_SECTION(synchronized_dispatch_name(dispatch_policy)) {
      auto dispatchers = std::make_shared<synchronized_async_dispatchers>(2U);
      dispatchers->warm();
      auto gates = std::make_shared<synchronized_pair_gate_registry>();
      auto graph = build_synchronized_async_fan_in_graph(synchronized_async_fan_in_shape::value,
                                                         dispatch_policy, dispatchers, gates);
      REQUIRE(graph.has_value());

      auto errors = run_synchronized_async_fan_in_wave(
          graph.value(), synchronized_async_fan_in_shape::value, 1, 128);
      if (!errors.empty()) {
        INFO(errors.front());
      }
      REQUIRE(errors.empty());
    }
  }
}

TEST_CASE("compose graph synchronized async value fan-in remains stable across "
          "repeated concurrent invokes",
          "[core][compose][graph][value][stress][concurrency][fanin][synchronized]"
          "[concurrent]") {
  constexpr std::array dispatch_policies{
      wh::compose::graph_dispatch_policy::same_wave,
      wh::compose::graph_dispatch_policy::next_wave,
  };

  for (const auto dispatch_policy : dispatch_policies) {
    DYNAMIC_SECTION(synchronized_dispatch_name(dispatch_policy)) {
      // Pin sibling branches to dedicated workers so the test exercises graph
      // fan-in semantics instead of anonymous pool fairness.
      auto dispatchers = std::make_shared<synchronized_async_dispatchers>(2U);
      dispatchers->warm();
      auto gates = std::make_shared<synchronized_pair_gate_registry>();
      auto graph = build_synchronized_async_fan_in_graph(synchronized_async_fan_in_shape::value,
                                                         dispatch_policy, dispatchers, gates);
      REQUIRE(graph.has_value());

      auto errors = run_synchronized_async_fan_in_wave(
          graph.value(), synchronized_async_fan_in_shape::value, 4, 32);
      if (!errors.empty()) {
        INFO(errors.front());
      }
      REQUIRE(errors.empty());
    }
  }
}

TEST_CASE("compose graph synchronized async stream fan-in remains stable across "
          "repeated concurrent invokes",
          "[core][compose][graph][stream][stress][concurrency][fanin][synchronized]"
          "[concurrent]") {
  constexpr std::array dispatch_policies{
      wh::compose::graph_dispatch_policy::same_wave,
      wh::compose::graph_dispatch_policy::next_wave,
  };

  for (const auto dispatch_policy : dispatch_policies) {
    DYNAMIC_SECTION(synchronized_dispatch_name(dispatch_policy)) {
      auto dispatchers = std::make_shared<synchronized_async_dispatchers>(2U);
      dispatchers->warm();
      auto gates = std::make_shared<synchronized_pair_gate_registry>();
      auto graph = build_synchronized_async_fan_in_graph(synchronized_async_fan_in_shape::stream,
                                                         dispatch_policy, dispatchers, gates);
      REQUIRE(graph.has_value());

      auto errors = run_synchronized_async_fan_in_wave(
          graph.value(), synchronized_async_fan_in_shape::stream, 4, 32);
      if (!errors.empty()) {
        INFO(errors.front());
      }
      REQUIRE(errors.empty());
    }
  }
}

TEST_CASE("compose graph repeated direct async stream collect waves remain stable",
          "[core][compose][graph][stream][stress][concurrency][collect][async]") {
  auto pool = std::make_shared<exec::static_thread_pool>(4U);
  auto graph = build_async_stream_collect_graph(pool);
  REQUIRE(graph.has_value());

  for (int round = 0; round < 8; ++round) {
    require_concurrent_graph_invoke_stable(
        graph.value(), [](const int input) { return 2 * input + 7; }, 8, 32);
  }
}

TEST_CASE("compose graph slow-fast matrix remains stable", "[core][compose][graph][stress][pace]") {
  constexpr std::array parallels{std::size_t{1U}, std::size_t{2U}};

  for (const auto mode : runtime_modes()) {
    for (const auto pace : all_pace_cases()) {
      for (const auto parallel : parallels) {
        DYNAMIC_SECTION(runtime_mode_name(mode)
                        << ":" << pace_name(pace) << ":parallel=" << parallel) {
          exec::static_thread_pool pool{4U};
          auto graph = build_paced_graph(pool.get_scheduler(), mode, pace, parallel);
          REQUIRE(graph.has_value());

          for (int iteration = 0; iteration < 8; ++iteration) {
            wh::core::run_context context{};
            auto output = invoke_value_sync(graph.value(), wh::core::any(iteration), context);
            REQUIRE(output.has_value());
            auto typed = read_any<int>(output.value());
            REQUIRE(typed.has_value());
            REQUIRE(typed.value() == 6 * iteration + 42);
          }
        }
      }
    }
  }
}

TEST_CASE("compose graph partial loop region matrix remains stable",
          "[core][compose][graph][stress][loop]") {
  constexpr std::array budgets{std::size_t{2U}, std::size_t{4U}};

  for (const auto mode : runtime_modes()) {
    for (const auto budget : budgets) {
      DYNAMIC_SECTION(runtime_mode_name(mode) << ":budget=" << budget) {
        auto graph = build_loop_region_graph(mode);
        if (mode == wh::compose::graph_runtime_mode::dag) {
          REQUIRE(graph.has_error());
          REQUIRE(graph.error() == wh::core::errc::contract_violation);
          continue;
        }

        REQUIRE(graph.has_value());
        wh::core::run_context context{};
        wh::compose::graph_call_options call_options{};
        call_options.pregel_max_steps = budget;
        auto status = invoke_graph_sync(graph.value(), wh::core::any(1), context, call_options, {});
        REQUIRE(status.has_value());
        REQUIRE(status.value().output_status.has_error());
        REQUIRE(status.value().output_status.error() == wh::core::errc::timeout);
        REQUIRE(status.value().report.step_limit_error.has_value());
        const auto &detail = *status.value().report.step_limit_error;
        REQUIRE(detail.budget == budget);
        REQUIRE(detail.step > detail.budget);
        REQUIRE(std::find(detail.completed_node_keys.begin(), detail.completed_node_keys.end(),
                          std::string{"loop_a"}) != detail.completed_node_keys.end());
      }
    }
  }
}

TEST_CASE("compose graph interrupt hook matrix remains stable",
          "[core][compose][graph][stress][interrupt]") {
  for (const auto mode : runtime_modes()) {
    for (const auto post_hook : {false, true}) {
      DYNAMIC_SECTION(runtime_mode_name(mode) << ":" << (post_hook ? "post_hook" : "pre_hook")) {
        wh::compose::graph_compile_options options{};
        options.mode = mode;
        wh::compose::graph graph{std::move(options)};
        REQUIRE(graph
                    .add_lambda("worker",
                                [](wh::compose::graph_value &input, wh::core::run_context &,
                                   const wh::compose::graph_call_scope &)
                                    -> wh::core::result<wh::compose::graph_value> {
                                  return std::move(input);
                                })
                    .has_value());
        REQUIRE(graph.add_entry_edge("worker").has_value());
        REQUIRE(graph.add_exit_edge("worker").has_value());
        REQUIRE(graph.compile().has_value());

        wh::core::run_context context{};
        const auto interrupt_id = std::string{post_hook ? "post-int" : "pre-int"} + "-" +
                                  std::string{runtime_mode_name(mode)};
        const auto hook = wh::compose::graph_interrupt_node_hook{
            [interrupt_id](const std::string_view node_key, const wh::compose::graph_value &payload,
                           wh::core::run_context &)
                -> wh::core::result<std::optional<wh::core::interrupt_signal>> {
              if (node_key != "worker") {
                return std::optional<wh::core::interrupt_signal>{};
              }
              return std::optional<wh::core::interrupt_signal>{wh::compose::make_interrupt_signal(
                  interrupt_id, wh::core::make_address({"graph", "worker"}), payload)};
            }};
        wh::compose::graph_invoke_controls controls{};
        if (post_hook) {
          controls.interrupt.post_hook = hook;
        } else {
          controls.interrupt.pre_hook = hook;
        }

        auto status = invoke_graph_sync(graph, wh::core::any(7), context, controls);
        REQUIRE(status.has_value());
        REQUIRE(status.value().output_status.has_error());
        REQUIRE(status.value().output_status.error() == wh::core::errc::canceled);
        REQUIRE(context.interrupt_info.has_value());
        REQUIRE(context.interrupt_info->interrupt_id == interrupt_id);
      }
    }
  }
}

TEST_CASE("compose graph checkpoint restore matrix remains stable",
          "[core][compose][graph][stress][checkpoint][restore]") {
  enum class restore_case : std::uint8_t {
    plain = 0U,
    immediate_rerun,
    wait_rerun,
  };

  const auto case_name = [](const restore_case value) noexcept -> std::string_view {
    switch (value) {
    case restore_case::plain:
      return "plain";
    case restore_case::immediate_rerun:
      return "immediate_rerun";
    case restore_case::wait_rerun:
      return "wait_rerun";
    }
    return "unknown";
  };

  constexpr std::array cases{restore_case::plain, restore_case::immediate_rerun,
                             restore_case::wait_rerun};

  for (const auto mode : runtime_modes()) {
    for (const auto item : cases) {
      DYNAMIC_SECTION(runtime_mode_name(mode) << ":" << case_name(item)) {
        if (item == restore_case::plain) {
          wh::compose::graph_compile_options options{};
          options.mode = mode;
          wh::compose::graph graph{std::move(options)};
          REQUIRE(graph
                      .add_lambda("inc",
                                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                                     const wh::compose::graph_call_scope &)
                                      -> wh::core::result<wh::compose::graph_value> {
                                    auto typed = read_any<int>(input);
                                    if (typed.has_error()) {
                                      return wh::core::result<wh::compose::graph_value>::failure(
                                          typed.error());
                                    }
                                    return wh::core::any(typed.value() + 1);
                                  })
                      .has_value());
          REQUIRE(graph.add_entry_edge("inc").has_value());
          REQUIRE(graph.add_exit_edge("inc").has_value());
          REQUIRE(graph.compile().has_value());

          wh::compose::checkpoint_store store{};
          const auto checkpoint_id =
              std::string{"stress-plain-"} + std::string{runtime_mode_name(mode)};
          wh::compose::checkpoint_save_options save_options{};
          save_options.checkpoint_id = checkpoint_id;

          wh::core::run_context capture_context{};
          auto checkpoint = capture_exact_checkpoint(graph, wh::core::any(10), capture_context);
          REQUIRE(checkpoint.has_value());
          checkpoint->checkpoint_id = checkpoint_id;
          REQUIRE(store.save(std::move(checkpoint).value(), save_options).has_value());

          auto persisted =
              store.load(wh::compose::checkpoint_load_options{.checkpoint_id = checkpoint_id});
          REQUIRE(persisted.has_value());
          REQUIRE(checkpoint_pending_input(persisted.value(), wh::compose::graph_start_node_key)
                      .value() == 10);

          wh::compose::graph_invoke_controls resume_controls{};
          resume_controls.checkpoint.load =
              wh::compose::checkpoint_load_options{.checkpoint_id = checkpoint_id};
          resume_controls.checkpoint.save = save_options;
          wh::core::run_context resume_context{};
          wh::compose::graph_runtime_services services{};
          services.checkpoint.store = std::addressof(store);
          auto resumed =
              invoke_graph_sync(graph, wh::compose::graph_input::restore_checkpoint(),
                                resume_context, resume_controls, std::addressof(services));
          REQUIRE(resumed.has_value());
          REQUIRE(resumed.value().output_status.has_value());
          REQUIRE(read_any<int>(resumed.value().output_status.value()).value() == 11);
          continue;
        }

        if (item == restore_case::immediate_rerun) {
          wh::compose::graph_compile_options options{};
          options.mode = mode;
          wh::compose::graph graph{std::move(options)};
          REQUIRE(graph
                      .add_lambda("worker",
                                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                                     const wh::compose::graph_call_scope &)
                                      -> wh::core::result<wh::compose::graph_value> {
                                    auto typed = read_any<int>(input);
                                    if (typed.has_error()) {
                                      return wh::core::result<wh::compose::graph_value>::failure(
                                          typed.error());
                                    }
                                    return wh::core::any(typed.value() + 1);
                                  })
                      .has_value());
          REQUIRE(graph.add_entry_edge("worker").has_value());
          REQUIRE(graph.add_exit_edge("worker").has_value());
          REQUIRE(graph.compile().has_value());

          wh::compose::checkpoint_store store{};
          const auto checkpoint_id =
              std::string{"stress-immediate-"} + std::string{runtime_mode_name(mode)};
          wh::compose::checkpoint_save_options save_options{};
          save_options.checkpoint_id = checkpoint_id;

          wh::core::run_context context{};
          context.interrupt_info = wh::core::interrupt_context{
              .interrupt_id = "stress-immediate",
              .location = wh::core::make_address({"graph", "worker"}),
              .state = wh::core::any{std::monostate{}},
          };
          wh::compose::graph_runtime_services services{};
          services.checkpoint.store = std::addressof(store);
          wh::compose::graph_invoke_controls controls{};
          controls.checkpoint.save = save_options;

          wh::compose::graph_call_options call_options{};
          call_options.external_interrupt_policy = wh::compose::graph_external_interrupt_policy{
              .timeout = std::chrono::milliseconds{0},
              .mode = wh::compose::graph_interrupt_timeout_mode::immediate_rerun,
              .auto_persist_external_interrupt = true,
              .manual_persist_internal_interrupt = true,
          };
          auto canceled = invoke_graph_sync(graph, wh::core::any(9), context, call_options,
                                            controls, std::addressof(services));
          REQUIRE(canceled.has_value());
          REQUIRE(canceled.value().output_status.has_error());
          REQUIRE(canceled.value().output_status.error() == wh::core::errc::canceled);

          auto persisted =
              store.load(wh::compose::checkpoint_load_options{.checkpoint_id = checkpoint_id});
          REQUIRE(persisted.has_value());
          REQUIRE(checkpoint_pending_input(persisted.value(), wh::compose::graph_start_node_key)
                      .value() == 9);
          REQUIRE(checkpoint_pending_input(persisted.value(), "worker").value() == 9);

          wh::compose::graph_invoke_controls resume_controls{};
          resume_controls.checkpoint.load =
              wh::compose::checkpoint_load_options{.checkpoint_id = checkpoint_id};
          resume_controls.checkpoint.save = save_options;
          wh::core::run_context resume_context{};
          auto resumed =
              invoke_graph_sync(graph, wh::compose::graph_input::restore_checkpoint(),
                                resume_context, resume_controls, std::addressof(services));
          REQUIRE(resumed.has_value());
          REQUIRE(resumed.value().output_status.has_error());
          REQUIRE(resumed.value().output_status.error() == wh::core::errc::canceled);
          REQUIRE(resume_context.interrupt_info.has_value());
          REQUIRE(resume_context.interrupt_info->interrupt_id == "stress-immediate");
          continue;
        }

        wh::compose::graph_compile_options options{};
        options.mode = mode;
        options.max_parallel_nodes = 1U;
        wh::compose::graph graph{std::move(options)};
        REQUIRE(graph
                    .add_lambda("slow",
                                [](const wh::compose::graph_value &input, wh::core::run_context &,
                                   const wh::compose::graph_call_scope &)
                                    -> wh::core::result<wh::compose::graph_value> {
                                  auto typed = read_any<int>(input);
                                  if (typed.has_error()) {
                                    return wh::core::result<wh::compose::graph_value>::failure(
                                        typed.error());
                                  }
                                  std::this_thread::sleep_for(std::chrono::milliseconds{4});
                                  return wh::core::any(typed.value());
                                })
                    .has_value());
        REQUIRE(graph
                    .add_lambda("tail",
                                [](const wh::compose::graph_value &input, wh::core::run_context &,
                                   const wh::compose::graph_call_scope &)
                                    -> wh::core::result<wh::compose::graph_value> {
                                  auto typed = read_any<int>(input);
                                  if (typed.has_error()) {
                                    return wh::core::result<wh::compose::graph_value>::failure(
                                        typed.error());
                                  }
                                  return wh::core::any(typed.value() + 1);
                                })
                    .has_value());
        REQUIRE(graph.add_entry_edge("slow").has_value());
        REQUIRE(graph.add_edge("slow", "tail").has_value());
        REQUIRE(graph.add_exit_edge("tail").has_value());
        REQUIRE(graph.compile().has_value());

        wh::compose::checkpoint_store store{};
        const auto checkpoint_id =
            std::string{"stress-wait-"} + std::string{runtime_mode_name(mode)};
        wh::compose::checkpoint_save_options save_options{};
        save_options.checkpoint_id = checkpoint_id;

        wh::core::run_context context{};
        context.interrupt_info = wh::core::interrupt_context{
            .interrupt_id = "stress-wait",
            .location = wh::core::make_address({"graph", "slow"}),
            .state = wh::core::any{std::monostate{}},
        };
        wh::compose::graph_runtime_services services{};
        services.checkpoint.store = std::addressof(store);
        wh::compose::graph_invoke_controls controls{};
        controls.checkpoint.save = save_options;

        wh::compose::graph_call_options call_options{};
        call_options.external_interrupt_policy = wh::compose::graph_external_interrupt_policy{
            .timeout = std::chrono::milliseconds{1},
            .mode = wh::compose::graph_interrupt_timeout_mode::wait_inflight,
            .auto_persist_external_interrupt = true,
            .manual_persist_internal_interrupt = true,
        };
        auto canceled = invoke_graph_sync(graph, wh::core::any(12), context, call_options, controls,
                                          std::addressof(services));
        REQUIRE(canceled.has_value());
        REQUIRE(canceled.value().output_status.has_error());
        REQUIRE(canceled.value().output_status.error() == wh::core::errc::canceled);

        auto persisted =
            store.load(wh::compose::checkpoint_load_options{.checkpoint_id = checkpoint_id});
        REQUIRE(persisted.has_value());
        auto persisted_start =
            checkpoint_pending_input(persisted.value(), wh::compose::graph_start_node_key);
        auto persisted_tail = checkpoint_pending_input(persisted.value(), "tail");
        REQUIRE(persisted_start.has_value());
        REQUIRE(persisted_start.value() == 12);
        REQUIRE(persisted_tail.has_value());
        REQUIRE(persisted_tail.value() == 12);

        wh::compose::graph_invoke_controls resume_controls{};
        resume_controls.checkpoint.load =
            wh::compose::checkpoint_load_options{.checkpoint_id = checkpoint_id};
        resume_controls.checkpoint.save = save_options;
        wh::core::run_context resume_context{};
        auto resumed = invoke_graph_sync(graph, wh::compose::graph_input::restore_checkpoint(),
                                         resume_context, resume_controls, std::addressof(services));
        REQUIRE(resumed.has_value());
        REQUIRE(resumed.value().output_status.has_error());
        REQUIRE(resumed.value().output_status.error() == wh::core::errc::canceled);
        REQUIRE(resume_context.interrupt_info.has_value());
        REQUIRE(resume_context.interrupt_info->interrupt_id == "stress-wait");
      }
    }
  }
}

TEST_CASE("compose nested graph checkpoint restore matrix remains stable",
          "[core][compose][graph][stress][checkpoint][restore][subgraph]") {
  enum class restore_case : std::uint8_t {
    matching_descendant = 0U,
    force_new_run,
  };

  const auto case_name = [](const restore_case value) noexcept -> std::string_view {
    switch (value) {
    case restore_case::matching_descendant:
      return "matching_descendant";
    case restore_case::force_new_run:
      return "force_new_run";
    }
    return "unknown";
  };

  constexpr std::array cases{restore_case::matching_descendant, restore_case::force_new_run};

  for (const auto mode : runtime_modes()) {
    for (const auto item : cases) {
      DYNAMIC_SECTION(runtime_mode_name(mode) << ":" << case_name(item)) {
        auto graphs_result = make_deep_nested_increment_graphs(mode);
        REQUIRE(graphs_result.has_value());
        auto graphs = std::move(graphs_result).value();

        std::size_t prepare_calls = 0U;
        wh::compose::checkpoint_backend backend{};
        backend.prepare_restore = wh::compose::checkpoint_backend_prepare_restore{
            [&prepare_calls](const wh::compose::checkpoint_load_options &, wh::core::run_context &)
                -> wh::core::result<wh::compose::checkpoint_restore_plan> {
              ++prepare_calls;
              return wh::compose::checkpoint_restore_plan{
                  .restore_from_checkpoint = false,
                  .checkpoint = std::nullopt,
              };
            }};
        backend.save = wh::compose::checkpoint_backend_save{
            [](wh::compose::checkpoint_state &&, wh::compose::checkpoint_save_options &&,
               wh::core::run_context &) -> wh::core::result<void> { return {}; }};

        wh::core::run_context middle_capture_context{};
        auto middle_checkpoint =
            capture_exact_checkpoint(graphs.middle, wh::core::any(7), middle_capture_context);
        REQUIRE(middle_checkpoint.has_value());
        middle_checkpoint->checkpoint_id =
            std::string{"middle-"} + std::string{runtime_mode_name(mode)};

        wh::core::run_context leaf_capture_context{};
        auto leaf_checkpoint =
            capture_exact_checkpoint(graphs.leaf, wh::core::any(7), leaf_capture_context);
        REQUIRE(leaf_checkpoint.has_value());
        leaf_checkpoint->checkpoint_id =
            std::string{"leaf-"} + std::string{runtime_mode_name(mode)};

        wh::compose::forwarded_checkpoint_map forwarded{};
        forwarded.emplace(graphs.middle_path_key, std::move(middle_checkpoint).value());
        forwarded.emplace(graphs.leaf_path_key, std::move(leaf_checkpoint).value());

        wh::core::run_context context{};
        wh::compose::graph_runtime_services services{};
        services.checkpoint.backend = std::addressof(backend);

        if (item == restore_case::matching_descendant) {
          wh::core::run_context ghost_capture_context{};
          auto ghost_checkpoint =
              capture_exact_checkpoint(graphs.leaf, wh::core::any(100), ghost_capture_context);
          REQUIRE(ghost_checkpoint.has_value());
          ghost_checkpoint->checkpoint_id = "ghost";
          forwarded.emplace("ghost_graph", std::move(ghost_checkpoint).value());

          wh::compose::graph_invoke_controls controls{};
          controls.checkpoint.forwarded_once = std::move(forwarded);
          auto invoked =
              invoke_graph_sync(graphs.parent, wh::compose::graph_input::restore_checkpoint(),
                                context, std::move(controls), std::addressof(services));
          REQUIRE(invoked.has_value());
          REQUIRE(invoked.value().output_status.has_value());
          REQUIRE(read_any<int>(invoked.value().output_status.value()).value() == 8);
          REQUIRE(prepare_calls == 1U);
          const auto &remaining = invoked.value().report.remaining_forwarded_checkpoint_keys;
          REQUIRE(remaining.size() == 1U);
          REQUIRE(std::find(remaining.begin(), remaining.end(), std::string{"ghost_graph"}) !=
                  remaining.end());
          continue;
        }

        wh::compose::checkpoint_load_options load_options{};
        load_options.force_new_run = true;
        wh::compose::graph_invoke_controls controls{};
        controls.checkpoint.load = load_options;
        controls.checkpoint.forwarded_once = std::move(forwarded);

        auto invoked = invoke_graph_sync(graphs.parent, wh::core::any(1), context,
                                         std::move(controls), std::addressof(services));
        REQUIRE(invoked.has_value());
        REQUIRE(invoked.value().output_status.has_value());
        REQUIRE(read_any<int>(invoked.value().output_status.value()).value() == 2);
        REQUIRE(prepare_calls == 0U);
        const auto &remaining = invoked.value().report.remaining_forwarded_checkpoint_keys;
        REQUIRE(remaining.size() == 2U);
        REQUIRE(std::find(remaining.begin(), remaining.end(), graphs.middle_path_key) !=
                remaining.end());
        REQUIRE(std::find(remaining.begin(), remaining.end(), graphs.leaf_path_key) !=
                remaining.end());
      }
    }
  }
}

TEST_CASE("compose detail collect stream pump remains stable across repeated concurrent waves",
          "[core][compose][graph][detail][stream][stress][concurrency]") {
  exec::static_thread_pool pool{4U};
  auto graph_scheduler = wh::core::detail::erase_resume_scheduler(pool.get_scheduler());

  const auto make_sender = [&graph_scheduler](const int base) -> wh::compose::graph_sender {
    auto reader = make_int_graph_stream({base, base + 1, base + 2});
    return make_collect_graph_sender(std::move(reader).value(), graph_scheduler);
  };

  for (int round = 0; round < 8; ++round) {
    auto errors = run_graph_sender_stress_wave(
        make_sender,
        [](const int base, wh::compose::graph_value output) -> std::optional<std::string> {
          auto sum = sum_collected_graph_chunks(output);
          if (sum.has_error()) {
            return std::string{"type error:"} + std::to_string(base);
          }
          const int expected = 3 * base + 3;
          if (sum.value() != expected) {
            return "mismatch:" + std::to_string(base) + ":" + std::to_string(sum.value()) + ":" +
                   std::to_string(expected);
          }
          return std::nullopt;
        },
        8, 32);
    if (!errors.empty()) {
      FAIL("round=" << round << " error_count=" << errors.size()
                    << " first_error=" << errors.front());
    }
    REQUIRE(errors.empty());
  }
}

TEST_CASE("compose detail child batch single collect lane remains stable",
          "[core][compose][graph][detail][batch][stress][concurrency]") {
  exec::static_thread_pool pool{4U};
  auto graph_scheduler = wh::core::detail::erase_resume_scheduler(pool.get_scheduler());

  struct batch_stage {
    std::vector<wh::compose::graph_value> collected{};
  };

  const auto make_sender = [&graph_scheduler](const int base) {
    std::vector<wh::compose::graph_sender> senders{};
    auto reader = make_int_graph_stream({base, base + 1});
    senders.push_back(make_collect_graph_sender(std::move(reader).value(), graph_scheduler));

    return wh::compose::detail::make_child_batch_sender(
        std::move(senders), batch_stage{},
        [](batch_stage &stage, const std::size_t,
           wh::core::result<wh::compose::graph_value> current) -> wh::core::result<void> {
          if (current.has_error()) {
            return wh::core::result<void>::failure(current.error());
          }
          stage.collected.push_back(std::move(current).value());
          return {};
        },
        [](batch_stage &&stage) -> wh::core::result<wh::compose::graph_value> {
          int sum = 0;
          for (auto &entry : stage.collected) {
            auto current = sum_collected_graph_chunks(entry);
            if (current.has_error()) {
              return wh::core::result<wh::compose::graph_value>::failure(current.error());
            }
            sum += current.value();
          }
          return wh::core::any(sum);
        },
        graph_scheduler);
  };

  for (int round = 0; round < 8; ++round) {
    auto errors = run_graph_sender_stress_wave(
        make_sender,
        [](const int base, wh::compose::graph_value output) -> std::optional<std::string> {
          auto typed = read_any<int>(std::move(output));
          if (typed.has_error()) {
            return std::string{"type error:"} + std::to_string(base);
          }
          const int expected = 2 * base + 1;
          if (typed.value() != expected) {
            return "mismatch:" + std::to_string(base) + ":" + std::to_string(typed.value()) + ":" +
                   std::to_string(expected);
          }
          return std::nullopt;
        },
        8, 32);
    INFO("round=" << round);
    if (!errors.empty()) {
      INFO(errors.front());
    }
    REQUIRE(errors.empty());
  }
}

TEST_CASE("compose detail child batch mixed collect lanes remains stable",
          "[core][compose][graph][detail][batch][stream][stress][concurrency]") {
  exec::static_thread_pool pool{4U};
  auto graph_scheduler = wh::core::detail::erase_resume_scheduler(pool.get_scheduler());

  struct batch_stage {
    std::vector<wh::compose::graph_value> collected{};
  };

  const auto make_sender = [&graph_scheduler](const int base) {
    std::vector<wh::compose::graph_sender> senders{};
    auto left = make_int_graph_stream({base, base + 1});
    auto right = make_int_graph_stream({base + 10, base + 11});
    senders.push_back(make_collect_graph_sender(std::move(left).value(), graph_scheduler));
    senders.push_back(make_collect_graph_sender(std::move(right).value(), graph_scheduler));

    return wh::compose::detail::make_child_batch_sender(
        std::move(senders), batch_stage{},
        [](batch_stage &stage, const std::size_t,
           wh::core::result<wh::compose::graph_value> current) -> wh::core::result<void> {
          if (current.has_error()) {
            return wh::core::result<void>::failure(current.error());
          }
          stage.collected.push_back(std::move(current).value());
          return {};
        },
        [](batch_stage &&stage) -> wh::core::result<wh::compose::graph_value> {
          int sum = 0;
          for (auto &entry : stage.collected) {
            auto current = sum_collected_graph_chunks(entry);
            if (current.has_error()) {
              return wh::core::result<wh::compose::graph_value>::failure(current.error());
            }
            sum += current.value();
          }
          return wh::core::any(sum);
        },
        graph_scheduler);
  };

  for (int round = 0; round < 8; ++round) {
    auto errors = run_graph_sender_stress_wave(
        make_sender,
        [](const int base, wh::compose::graph_value output) -> std::optional<std::string> {
          auto typed = read_any<int>(std::move(output));
          if (typed.has_error()) {
            return std::string{"type error:"} + std::to_string(base);
          }
          const int expected = 4 * base + 22;
          if (typed.value() != expected) {
            return "mismatch:" + std::to_string(base) + ":" + std::to_string(typed.value()) + ":" +
                   std::to_string(expected);
          }
          return std::nullopt;
        },
        8, 32);
    INFO("round=" << round);
    if (!errors.empty()) {
      INFO(errors.front());
    }
    REQUIRE(errors.empty());
  }
}

TEST_CASE("compose graph minimal concurrent stream fan-in remains stable in next-wave mode",
          "[core][compose][graph][stream][stress][concurrency][fanin][next-wave]") {
  auto pool = std::make_shared<exec::static_thread_pool>(4U);

  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::dag;
  options.dispatch_policy = wh::compose::graph_dispatch_policy::next_wave;
  options.max_parallel_nodes = 2U;
  options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
  options.fan_in_policy = wh::compose::graph_fan_in_policy::require_all_sources;
  wh::compose::graph graph{std::move(options)};

  auto make_async_source = [pool](const int bias) {
    return [pool, bias](const wh::compose::graph_value &input, wh::core::run_context &,
                        const wh::compose::graph_call_scope &) {
      auto copied_input = input;
      return stdexec::starts_on(
          pool->get_scheduler(),
          stdexec::just(std::move(copied_input)) |
              stdexec::then([bias](wh::compose::graph_value payload)
                                -> wh::core::result<wh::compose::graph_stream_reader> {
                auto typed = read_any<int>(std::move(payload));
                if (typed.has_error()) {
                  return wh::core::result<wh::compose::graph_stream_reader>::failure(typed.error());
                }
                return make_int_graph_stream({typed.value() + bias, typed.value() + bias + 1});
              }));
    };
  };

  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream,
                          wh::compose::node_exec_mode::async>("left_source", make_async_source(0))
              .has_value());
  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream,
                          wh::compose::node_exec_mode::async>("right_source", make_async_source(10))
              .has_value());
  REQUIRE(graph
              .add_lambda(
                  "merged_value",
                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    auto merged = read_any<wh::compose::graph_value_map>(input);
                    if (merged.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(merged.error());
                    }
                    int total = 0;
                    for (const auto &entry : merged.value()) {
                      auto chunks = sum_collected_graph_chunks(entry.second);
                      if (chunks.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(chunks.error());
                      }
                      total += chunks.value();
                    }
                    return wh::core::any(total);
                  })
              .has_value());
  REQUIRE(graph.add_entry_edge("left_source").has_value());
  REQUIRE(graph.add_entry_edge("right_source").has_value());
  REQUIRE(
      graph.add_edge("left_source", "merged_value", make_auto_contract_edge_options()).has_value());
  REQUIRE(graph.add_edge("right_source", "merged_value", make_auto_contract_edge_options())
              .has_value());
  REQUIRE(graph.add_exit_edge("merged_value").has_value());
  REQUIRE(graph.compile().has_value());

  auto run_wave = [&graph](const int worker_count, const int iterations_per_worker,
                           const int input_bias = 0) -> std::vector<std::string> {
    std::atomic<bool> failed{false};
    std::mutex error_mutex{};
    std::vector<std::string> errors{};
    std::vector<std::thread> workers{};
    workers.reserve(static_cast<std::size_t>(worker_count));

    for (int worker_id = 0; worker_id < worker_count; ++worker_id) {
      workers.emplace_back([&, worker_id]() {
        try {
          for (int iteration = 0; iteration < iterations_per_worker; ++iteration) {
            const int input = input_bias + worker_id * 100 + iteration;
            wh::core::run_context context{};
            auto invoked = invoke_value_sync(graph, wh::core::any(input), context);
            if (invoked.has_error()) {
              std::lock_guard lock{error_mutex};
              failed.store(true, std::memory_order_release);
              errors.push_back("invoke error:" + std::to_string(input));
              return;
            }
            auto typed = read_any<int>(std::move(invoked).value());
            if (typed.has_error()) {
              std::lock_guard lock{error_mutex};
              failed.store(true, std::memory_order_release);
              errors.push_back("type error:" + std::to_string(input));
              return;
            }
            const int expected = 4 * input + 22;
            if (typed.value() != expected) {
              std::lock_guard lock{error_mutex};
              failed.store(true, std::memory_order_release);
              errors.push_back("mismatch:" + std::to_string(input) + ":" +
                               std::to_string(typed.value()) + ":" + std::to_string(expected));
              return;
            }
          }
        } catch (const std::exception &error) {
          std::lock_guard lock{error_mutex};
          failed.store(true, std::memory_order_release);
          errors.push_back("exception:" + std::string{error.what()});
        } catch (...) {
          std::lock_guard lock{error_mutex};
          failed.store(true, std::memory_order_release);
          errors.push_back("exception:unknown");
        }
      });
    }

    for (auto &worker : workers) {
      worker.join();
    }

    if (!failed.load(std::memory_order_acquire)) {
      return {};
    }
    return errors;
  };

  for (int round = 0; round < 8; ++round) {
    auto errors = run_wave(8, 32, round * 10000);
    INFO("round=" << round);
    if (!errors.empty()) {
      INFO(errors.front());
    }
    REQUIRE(errors.empty());
  }
}

TEST_CASE("compose graph minimal concurrent value fan-in remains stable",
          "[core][compose][graph][value][stress][concurrency][fanin]") {
  auto pool = std::make_shared<exec::static_thread_pool>(4U);

  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::dag;
  options.max_parallel_nodes = 2U;
  options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
  wh::compose::graph graph{std::move(options)};

  auto make_async_source = [pool](const int bias) {
    return [pool, bias](const wh::compose::graph_value &input, wh::core::run_context &,
                        const wh::compose::graph_call_scope &) {
      auto copied_input = input;
      return stdexec::starts_on(
          pool->get_scheduler(),
          stdexec::just(std::move(copied_input)) |
              stdexec::then([bias](wh::compose::graph_value payload)
                                -> wh::core::result<wh::compose::graph_value> {
                auto typed = read_any<int>(std::move(payload));
                if (typed.has_error()) {
                  return wh::core::result<wh::compose::graph_value>::failure(typed.error());
                }
                return wh::core::any(typed.value() + bias);
              }));
    };
  };

  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::value,
                          wh::compose::node_exec_mode::async>("left_source", make_async_source(0))
              .has_value());
  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::value,
                          wh::compose::node_exec_mode::async>("right_source", make_async_source(10))
              .has_value());
  REQUIRE(graph
              .add_lambda(
                  "merged_value",
                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    auto merged = read_any<wh::compose::graph_value_map>(input);
                    if (merged.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(merged.error());
                    }
                    int total = 0;
                    for (const auto &entry : merged.value()) {
                      auto typed = read_any<int>(entry.second);
                      if (typed.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(typed.error());
                      }
                      total += typed.value();
                    }
                    return wh::core::any(total);
                  })
              .has_value());
  REQUIRE(graph.add_entry_edge("left_source").has_value());
  REQUIRE(graph.add_entry_edge("right_source").has_value());
  REQUIRE(graph.add_edge("left_source", "merged_value").has_value());
  REQUIRE(graph.add_edge("right_source", "merged_value").has_value());
  REQUIRE(graph.add_exit_edge("merged_value").has_value());
  REQUIRE(graph.compile().has_value());

  auto run_wave = [&graph](const int worker_count, const int iterations_per_worker,
                           const int input_bias = 0) -> std::vector<std::string> {
    std::atomic<bool> failed{false};
    std::mutex error_mutex{};
    std::vector<std::string> errors{};
    std::vector<std::thread> workers{};
    workers.reserve(static_cast<std::size_t>(worker_count));

    for (int worker_id = 0; worker_id < worker_count; ++worker_id) {
      workers.emplace_back([&, worker_id]() {
        try {
          for (int iteration = 0; iteration < iterations_per_worker; ++iteration) {
            const int input = input_bias + worker_id * 100 + iteration;
            wh::core::run_context context{};
            auto invoked = invoke_value_sync(graph, wh::core::any(input), context);
            if (invoked.has_error()) {
              std::lock_guard lock{error_mutex};
              failed.store(true, std::memory_order_release);
              errors.push_back("invoke error:" + std::to_string(input));
              return;
            }
            auto typed = read_any<int>(std::move(invoked).value());
            if (typed.has_error()) {
              std::lock_guard lock{error_mutex};
              failed.store(true, std::memory_order_release);
              errors.push_back("type error:" + std::to_string(input));
              return;
            }
            const int expected = 2 * input + 10;
            if (typed.value() != expected) {
              std::lock_guard lock{error_mutex};
              failed.store(true, std::memory_order_release);
              errors.push_back("mismatch:" + std::to_string(input) + ":" +
                               std::to_string(typed.value()) + ":" + std::to_string(expected));
              return;
            }
          }
        } catch (const std::exception &error) {
          std::lock_guard lock{error_mutex};
          failed.store(true, std::memory_order_release);
          errors.push_back("exception:" + std::string{error.what()});
        } catch (...) {
          std::lock_guard lock{error_mutex};
          failed.store(true, std::memory_order_release);
          errors.push_back("exception:unknown");
        }
      });
    }

    for (auto &worker : workers) {
      worker.join();
    }

    if (!failed.load(std::memory_order_acquire)) {
      return {};
    }
    return errors;
  };

  for (int round = 0; round < 8; ++round) {
    auto errors = run_wave(8, 32, round * 10000);
    INFO("round=" << round);
    if (!errors.empty()) {
      INFO(errors.front());
    }
    REQUIRE(errors.empty());
  }
}

TEST_CASE("compose graph immediate value fan-in remains stable across repeated concurrent waves",
          "[core][compose][graph][value][stress][concurrency][fanin][inline]") {
  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::dag;
  options.max_parallel_nodes = 2U;
  options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
  wh::compose::graph graph{std::move(options)};

  auto make_source = [](const int bias) {
    return
        [bias](
            const wh::compose::graph_value &input, wh::core::run_context &,
            const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
          auto typed = read_any<int>(input);
          if (typed.has_error()) {
            return wh::core::result<wh::compose::graph_value>::failure(typed.error());
          }
          return wh::core::any(typed.value() + bias);
        };
  };

  REQUIRE(graph.add_lambda("left_source", make_source(0)).has_value());
  REQUIRE(graph.add_lambda("right_source", make_source(10)).has_value());
  REQUIRE(graph
              .add_lambda(
                  "merged_value",
                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    auto merged = read_any<wh::compose::graph_value_map>(input);
                    if (merged.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(merged.error());
                    }
                    int total = 0;
                    for (const auto &entry : merged.value()) {
                      auto typed = read_any<int>(entry.second);
                      if (typed.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(typed.error());
                      }
                      total += typed.value();
                    }
                    return wh::core::any(total);
                  })
              .has_value());
  REQUIRE(graph.add_entry_edge("left_source").has_value());
  REQUIRE(graph.add_entry_edge("right_source").has_value());
  REQUIRE(graph.add_edge("left_source", "merged_value").has_value());
  REQUIRE(graph.add_edge("right_source", "merged_value").has_value());
  REQUIRE(graph.add_exit_edge("merged_value").has_value());
  REQUIRE(graph.compile().has_value());

  auto run_wave = [&graph](const int worker_count, const int iterations_per_worker,
                           const int input_bias = 0) -> std::vector<std::string> {
    std::atomic<bool> failed{false};
    std::mutex error_mutex{};
    std::vector<std::string> errors{};
    std::vector<std::thread> workers{};
    workers.reserve(static_cast<std::size_t>(worker_count));

    for (int worker_id = 0; worker_id < worker_count; ++worker_id) {
      workers.emplace_back([&, worker_id]() {
        try {
          for (int iteration = 0; iteration < iterations_per_worker; ++iteration) {
            const int input = input_bias + worker_id * 100 + iteration;
            wh::core::run_context context{};
            auto invoked = invoke_value_sync(graph, wh::core::any(input), context);
            if (invoked.has_error()) {
              std::lock_guard lock{error_mutex};
              failed.store(true, std::memory_order_release);
              errors.push_back("invoke error:" + std::to_string(input));
              return;
            }
            auto typed = read_any<int>(std::move(invoked).value());
            if (typed.has_error()) {
              std::lock_guard lock{error_mutex};
              failed.store(true, std::memory_order_release);
              errors.push_back("type error:" + std::to_string(input));
              return;
            }
            const int expected = 2 * input + 10;
            if (typed.value() != expected) {
              std::lock_guard lock{error_mutex};
              failed.store(true, std::memory_order_release);
              errors.push_back("mismatch:" + std::to_string(input) + ":" +
                               std::to_string(typed.value()) + ":" + std::to_string(expected));
              return;
            }
          }
        } catch (const std::exception &error) {
          std::lock_guard lock{error_mutex};
          failed.store(true, std::memory_order_release);
          errors.push_back("exception:" + std::string{error.what()});
        } catch (...) {
          std::lock_guard lock{error_mutex};
          failed.store(true, std::memory_order_release);
          errors.push_back("exception:unknown");
        }
      });
    }

    for (auto &worker : workers) {
      worker.join();
    }

    if (!failed.load(std::memory_order_acquire)) {
      return {};
    }
    return errors;
  };

  for (int round = 0; round < 8; ++round) {
    auto errors = run_wave(8, 32, round * 10000);
    INFO("round=" << round);
    if (!errors.empty()) {
      INFO(errors.front());
    }
    REQUIRE(errors.empty());
  }
}

TEST_CASE("compose graph minimal concurrent stream fan-in remains stable in same-wave mode",
          "[core][compose][graph][stream][stress][concurrency][fanin][same-wave]") {
  auto pool = std::make_shared<exec::static_thread_pool>(4U);

  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::dag;
  options.dispatch_policy = wh::compose::graph_dispatch_policy::same_wave;
  options.max_parallel_nodes = 2U;
  options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
  options.fan_in_policy = wh::compose::graph_fan_in_policy::require_all_sources;
  wh::compose::graph graph{std::move(options)};

  auto make_async_source = [pool](const int bias) {
    return [pool, bias](const wh::compose::graph_value &input, wh::core::run_context &,
                        const wh::compose::graph_call_scope &) {
      auto copied_input = input;
      return stdexec::starts_on(
          pool->get_scheduler(),
          stdexec::just(std::move(copied_input)) |
              stdexec::then([bias](wh::compose::graph_value payload)
                                -> wh::core::result<wh::compose::graph_stream_reader> {
                auto typed = read_any<int>(std::move(payload));
                if (typed.has_error()) {
                  return wh::core::result<wh::compose::graph_stream_reader>::failure(typed.error());
                }
                return make_int_graph_stream({typed.value() + bias, typed.value() + bias + 1});
              }));
    };
  };

  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream,
                          wh::compose::node_exec_mode::async>("left_source", make_async_source(0))
              .has_value());
  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream,
                          wh::compose::node_exec_mode::async>("right_source", make_async_source(10))
              .has_value());
  REQUIRE(graph
              .add_lambda(
                  "merged_value",
                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    auto merged = read_any<wh::compose::graph_value_map>(input);
                    if (merged.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(merged.error());
                    }
                    int total = 0;
                    for (const auto &entry : merged.value()) {
                      auto chunks = sum_collected_graph_chunks(entry.second);
                      if (chunks.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(chunks.error());
                      }
                      total += chunks.value();
                    }
                    return wh::core::any(total);
                  })
              .has_value());
  REQUIRE(graph.add_entry_edge("left_source").has_value());
  REQUIRE(graph.add_entry_edge("right_source").has_value());
  REQUIRE(
      graph.add_edge("left_source", "merged_value", make_auto_contract_edge_options()).has_value());
  REQUIRE(graph.add_edge("right_source", "merged_value", make_auto_contract_edge_options())
              .has_value());
  REQUIRE(graph.add_exit_edge("merged_value").has_value());
  REQUIRE(graph.compile().has_value());

  auto run_wave = [&graph](const int worker_count, const int iterations_per_worker,
                           const int input_bias = 0) -> std::vector<std::string> {
    std::atomic<bool> failed{false};
    std::mutex error_mutex{};
    std::vector<std::string> errors{};
    std::vector<std::thread> workers{};
    workers.reserve(static_cast<std::size_t>(worker_count));

    for (int worker_id = 0; worker_id < worker_count; ++worker_id) {
      workers.emplace_back([&, worker_id]() {
        try {
          for (int iteration = 0; iteration < iterations_per_worker; ++iteration) {
            const int input = input_bias + worker_id * 100 + iteration;
            wh::core::run_context context{};
            auto invoked = invoke_value_sync(graph, wh::core::any(input), context);
            if (invoked.has_error()) {
              std::lock_guard lock{error_mutex};
              failed.store(true, std::memory_order_release);
              errors.push_back("invoke error:" + std::to_string(input));
              return;
            }
            auto typed = read_any<int>(std::move(invoked).value());
            if (typed.has_error()) {
              std::lock_guard lock{error_mutex};
              failed.store(true, std::memory_order_release);
              errors.push_back("type error:" + std::to_string(input));
              return;
            }
            const int expected = 4 * input + 22;
            if (typed.value() != expected) {
              std::lock_guard lock{error_mutex};
              failed.store(true, std::memory_order_release);
              errors.push_back("mismatch:" + std::to_string(input) + ":" +
                               std::to_string(typed.value()) + ":" + std::to_string(expected));
              return;
            }
          }
        } catch (const std::exception &error) {
          std::lock_guard lock{error_mutex};
          failed.store(true, std::memory_order_release);
          errors.push_back("exception:" + std::string{error.what()});
        } catch (...) {
          std::lock_guard lock{error_mutex};
          failed.store(true, std::memory_order_release);
          errors.push_back("exception:unknown");
        }
      });
    }

    for (auto &worker : workers) {
      worker.join();
    }

    if (!failed.load(std::memory_order_acquire)) {
      return {};
    }
    return errors;
  };

  for (int round = 0; round < 8; ++round) {
    auto errors = run_wave(8, 32, round * 10000);
    INFO("round=" << round);
    if (!errors.empty()) {
      INFO(errors.front());
    }
    REQUIRE(errors.empty());
  }
}

TEST_CASE("compose graph repeated direct sync stream collect waves remain stable",
          "[core][compose][graph][stream][stress][concurrency][collect][sync]") {
  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::dag;
  options.max_parallel_nodes = 1U;
  wh::compose::graph graph{std::move(options)};

  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream>(
                  "source",
                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_stream_reader> {
                    auto typed = read_any<int>(input);
                    if (typed.has_error()) {
                      return wh::core::result<wh::compose::graph_stream_reader>::failure(
                          typed.error());
                    }
                    return make_int_graph_stream({typed.value() + 3, typed.value() + 4});
                  })
              .has_value());
  REQUIRE(
      graph
          .add_component(
              wh::compose::make_component_node<
                  wh::core::component_kind::custom, wh::compose::node_contract::value,
                  wh::compose::node_contract::value, std::vector<wh::compose::graph_value>, int>(
                  "consumer", collected_sum_consumer{}))
          .has_value());
  REQUIRE(graph.add_entry_edge("source").has_value());
  REQUIRE(graph.add_edge("source", "consumer").has_value());
  REQUIRE(graph.add_exit_edge("consumer").has_value());
  REQUIRE(graph.compile().has_value());

  auto run_wave = [&graph](const int worker_count, const int iterations_per_worker,
                           const int input_bias = 0) -> std::vector<std::string> {
    std::atomic<bool> failed{false};
    std::mutex error_mutex{};
    std::vector<std::string> errors{};
    std::vector<std::thread> workers{};
    workers.reserve(static_cast<std::size_t>(worker_count));

    for (int worker_id = 0; worker_id < worker_count; ++worker_id) {
      workers.emplace_back([&, worker_id]() {
        try {
          for (int iteration = 0; iteration < iterations_per_worker; ++iteration) {
            const int input = input_bias + worker_id * 100 + iteration;
            wh::core::run_context context{};
            auto invoked = invoke_value_sync(graph, wh::core::any(input), context);
            if (invoked.has_error()) {
              std::lock_guard lock{error_mutex};
              failed.store(true, std::memory_order_release);
              errors.push_back("invoke error:" + std::to_string(input));
              return;
            }
            auto typed = read_any<int>(std::move(invoked).value());
            if (typed.has_error()) {
              std::lock_guard lock{error_mutex};
              failed.store(true, std::memory_order_release);
              errors.push_back("type error:" + std::to_string(input));
              return;
            }
            const int expected = 2 * input + 7;
            if (typed.value() != expected) {
              std::lock_guard lock{error_mutex};
              failed.store(true, std::memory_order_release);
              errors.push_back("mismatch:" + std::to_string(input) + ":" +
                               std::to_string(typed.value()) + ":" + std::to_string(expected));
              return;
            }
          }
        } catch (const std::exception &error) {
          std::lock_guard lock{error_mutex};
          failed.store(true, std::memory_order_release);
          errors.push_back("exception:" + std::string{error.what()});
        } catch (...) {
          std::lock_guard lock{error_mutex};
          failed.store(true, std::memory_order_release);
          errors.push_back("exception:unknown");
        }
      });
    }

    for (auto &worker : workers) {
      worker.join();
    }

    if (!failed.load(std::memory_order_acquire)) {
      return {};
    }
    return errors;
  };

  for (int round = 0; round < 8; ++round) {
    auto errors = run_wave(8, 32, round * 10000);
    INFO("round=" << round);
    if (!errors.empty()) {
      INFO(errors.front());
    }
    REQUIRE(errors.empty());
  }
}

struct concurrent_mixed_stream_fixture {
  std::shared_ptr<exec::static_thread_pool> pool{};
  wh::compose::graph graph{};
};

[[nodiscard]] auto make_concurrent_mixed_stream_fixture() -> concurrent_mixed_stream_fixture {
  concurrent_mixed_stream_fixture fixture{};
  fixture.pool = std::make_shared<exec::static_thread_pool>(4U);

  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::dag;
  options.max_parallel_nodes = 2U;
  options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
  options.fan_in_policy = wh::compose::graph_fan_in_policy::require_all_sources;
  fixture.graph = wh::compose::graph{std::move(options)};

  auto &graph = fixture.graph;
  auto make_async_source = [pool = fixture.pool](const int bias) {
    return [pool, bias](const wh::compose::graph_value &input, wh::core::run_context &,
                        const wh::compose::graph_call_scope &) {
      auto copied_input = input;
      return stdexec::starts_on(
          pool->get_scheduler(),
          stdexec::just(std::move(copied_input)) |
              stdexec::then([bias](wh::compose::graph_value payload)
                                -> wh::core::result<wh::compose::graph_stream_reader> {
                auto typed = read_any<int>(std::move(payload));
                if (typed.has_error()) {
                  return wh::core::result<wh::compose::graph_stream_reader>::failure(typed.error());
                }
                return make_int_graph_stream({typed.value() + bias, typed.value() + bias + 1});
              }));
    };
  };

  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream,
                          wh::compose::node_exec_mode::async>("left_source", make_async_source(0))
              .has_value());
  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream,
                          wh::compose::node_exec_mode::async>("right_source", make_async_source(10))
              .has_value());
  REQUIRE(graph
              .add_lambda(
                  "merged_value",
                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    auto merged = read_any<wh::compose::graph_value_map>(input);
                    if (merged.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(merged.error());
                    }
                    int total = 0;
                    for (const auto &entry : merged.value()) {
                      auto chunks = sum_collected_graph_chunks(entry.second);
                      if (chunks.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(chunks.error());
                      }
                      total += chunks.value();
                    }
                    return wh::core::any(total);
                  })
              .has_value());
  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::stream, wh::compose::node_contract::value>(
                  "count_sink",
                  [](wh::compose::graph_stream_reader input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    auto values = collect_int_graph_stream(std::move(input));
                    if (values.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(values.error());
                    }
                    return wh::core::any(static_cast<int>(values.value().size()));
                  })
              .has_value());
  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::stream, wh::compose::node_contract::value>(
                  "sum_sink",
                  [](wh::compose::graph_stream_reader input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    auto values = collect_int_graph_stream(std::move(input));
                    if (values.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(values.error());
                    }
                    int sum = 0;
                    for (const auto value : values.value()) {
                      sum += value;
                    }
                    return wh::core::any(sum);
                  })
              .has_value());
  REQUIRE(graph
              .add_lambda(
                  "final_join",
                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    auto merged = read_any<wh::compose::graph_value_map>(input);
                    if (merged.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(merged.error());
                    }
                    auto count = read_any<int>(merged.value().at("count_sink"));
                    auto sum = read_any<int>(merged.value().at("sum_sink"));
                    if (count.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(count.error());
                    }
                    if (sum.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(sum.error());
                    }
                    return wh::core::any(count.value() + sum.value());
                  })
              .has_value());

  REQUIRE(graph.add_entry_edge("left_source").has_value());
  REQUIRE(graph.add_entry_edge("right_source").has_value());
  REQUIRE(
      graph.add_edge("left_source", "merged_value", make_auto_contract_edge_options()).has_value());
  REQUIRE(graph.add_edge("right_source", "merged_value", make_auto_contract_edge_options())
              .has_value());
  REQUIRE(
      graph.add_edge("merged_value", "count_sink", make_auto_contract_edge_options()).has_value());
  REQUIRE(
      graph.add_edge("merged_value", "sum_sink", make_auto_contract_edge_options()).has_value());
  REQUIRE(graph.add_edge("count_sink", "final_join").has_value());
  REQUIRE(graph.add_edge("sum_sink", "final_join").has_value());
  REQUIRE(graph.add_exit_edge("final_join").has_value());
  REQUIRE(graph.compile().has_value());

  return fixture;
}

struct direct_stream_collect_fixture {
  std::shared_ptr<exec::static_thread_pool> pool{};
  wh::compose::graph graph{};
};

[[nodiscard]] auto make_direct_stream_collect_fixture() -> direct_stream_collect_fixture {
  direct_stream_collect_fixture fixture{};
  fixture.pool = std::make_shared<exec::static_thread_pool>(4U);

  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::dag;
  options.max_parallel_nodes = 1U;
  fixture.graph = wh::compose::graph{std::move(options)};

  auto &graph = fixture.graph;
  auto make_async_source = [pool = fixture.pool](const int bias) {
    return [pool, bias](const wh::compose::graph_value &input, wh::core::run_context &,
                        const wh::compose::graph_call_scope &) {
      auto copied_input = input;
      return stdexec::starts_on(
          pool->get_scheduler(),
          stdexec::just(std::move(copied_input)) |
              stdexec::then([bias](wh::compose::graph_value payload)
                                -> wh::core::result<wh::compose::graph_stream_reader> {
                auto typed = read_any<int>(std::move(payload));
                if (typed.has_error()) {
                  return wh::core::result<wh::compose::graph_stream_reader>::failure(typed.error());
                }
                return make_int_graph_stream({typed.value() + bias, typed.value() + bias + 1});
              }));
    };
  };

  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream,
                          wh::compose::node_exec_mode::async>("source", make_async_source(3))
              .has_value());
  REQUIRE(
      graph
          .add_component(
              wh::compose::make_component_node<
                  wh::core::component_kind::custom, wh::compose::node_contract::value,
                  wh::compose::node_contract::value, std::vector<wh::compose::graph_value>, int>(
                  "consumer", collected_sum_consumer{}))
          .has_value());
  REQUIRE(graph.add_entry_edge("source").has_value());
  REQUIRE(graph.add_edge("source", "consumer").has_value());
  REQUIRE(graph.add_exit_edge("consumer").has_value());
  REQUIRE(graph.compile().has_value());

  return fixture;
}

[[nodiscard]] auto
run_direct_stream_collect_wave(const wh::compose::graph &graph, const int worker_count,
                               const int iterations_per_worker, const int input_bias = 0)
    -> std::vector<std::string> {
  std::atomic<bool> failed{false};
  std::mutex error_mutex{};
  std::vector<std::string> errors{};
  std::vector<std::thread> workers{};
  workers.reserve(static_cast<std::size_t>(worker_count));

  for (int worker_id = 0; worker_id < worker_count; ++worker_id) {
    workers.emplace_back([&, worker_id]() {
      try {
        for (int iteration = 0; iteration < iterations_per_worker; ++iteration) {
          const int input = input_bias + worker_id * 100 + iteration;
          wh::core::run_context context{};
          auto invoked = invoke_value_sync(graph, wh::core::any(input), context);
          if (invoked.has_error()) {
            std::lock_guard lock{error_mutex};
            failed.store(true, std::memory_order_release);
            errors.push_back("invoke error:" + std::to_string(input));
            return;
          }
          auto typed = read_any<int>(invoked.value());
          if (typed.has_error()) {
            std::lock_guard lock{error_mutex};
            failed.store(true, std::memory_order_release);
            errors.push_back("type error:" + std::to_string(input));
            return;
          }
          const int expected = 2 * input + 7;
          if (typed.value() != expected) {
            std::lock_guard lock{error_mutex};
            failed.store(true, std::memory_order_release);
            errors.push_back("mismatch:" + std::to_string(input) + ":" +
                             std::to_string(typed.value()) + ":" + std::to_string(expected));
            return;
          }
        }
      } catch (const std::exception &error) {
        std::lock_guard lock{error_mutex};
        failed.store(true, std::memory_order_release);
        errors.push_back("exception:" + std::string{error.what()});
      } catch (...) {
        std::lock_guard lock{error_mutex};
        failed.store(true, std::memory_order_release);
        errors.push_back("exception:unknown");
      }
    });
  }

  for (auto &worker : workers) {
    worker.join();
  }

  if (!failed.load(std::memory_order_acquire)) {
    return {};
  }
  return errors;
}

[[nodiscard]] auto
run_concurrent_mixed_stream_wave(const wh::compose::graph &graph, const int worker_count,
                                 const int iterations_per_worker, const int input_bias = 0)
    -> std::vector<std::string> {
  std::atomic<bool> failed{false};
  std::mutex error_mutex{};
  std::vector<std::string> errors{};
  std::vector<std::thread> workers{};
  workers.reserve(static_cast<std::size_t>(worker_count));

  for (int worker_id = 0; worker_id < worker_count; ++worker_id) {
    workers.emplace_back([&, worker_id]() {
      try {
        for (int iteration = 0; iteration < iterations_per_worker; ++iteration) {
          const int input = input_bias + worker_id * 100 + iteration;
          wh::core::run_context context{};
          auto invoked = invoke_value_sync(graph, wh::core::any(input), context);
          if (invoked.has_error()) {
            std::lock_guard lock{error_mutex};
            failed.store(true, std::memory_order_release);
            errors.push_back("invoke error:" + std::to_string(input));
            return;
          }
          auto typed = read_any<int>(invoked.value());
          if (typed.has_error()) {
            std::lock_guard lock{error_mutex};
            failed.store(true, std::memory_order_release);
            errors.push_back("type error:" + std::to_string(input));
            return;
          }
          const int expected = 4 * input + 23;
          if (typed.value() != expected) {
            std::lock_guard lock{error_mutex};
            failed.store(true, std::memory_order_release);
            errors.push_back("mismatch:" + std::to_string(input) + ":" +
                             std::to_string(typed.value()) + ":" + std::to_string(expected));
            return;
          }
        }
      } catch (const std::exception &error) {
        std::lock_guard lock{error_mutex};
        failed.store(true, std::memory_order_release);
        errors.push_back("exception:" + std::string{error.what()});
      } catch (...) {
        std::lock_guard lock{error_mutex};
        failed.store(true, std::memory_order_release);
        errors.push_back("exception:unknown");
      }
    });
  }

  for (auto &worker : workers) {
    worker.join();
  }

  if (!failed.load(std::memory_order_acquire)) {
    return {};
  }
  return errors;
}

TEST_CASE("compose graph concurrent mixed stream invoke remains stable",
          "[core][compose][graph][stream][stress]") {
  auto fixture = make_concurrent_mixed_stream_fixture();
  auto errors = run_concurrent_mixed_stream_wave(fixture.graph, 8, 32);
  if (!errors.empty()) {
    INFO(errors.front());
  }
  REQUIRE(errors.empty());
}

TEST_CASE("compose graph repeated concurrent mixed stream waves remain stable",
          "[core][compose][graph][stream][stress][concurrency]") {
  auto fixture = make_concurrent_mixed_stream_fixture();

  for (int round = 0; round < 8; ++round) {
    auto errors = run_concurrent_mixed_stream_wave(fixture.graph, 8, 32, round * 10000);
    INFO("round=" << round);
    if (!errors.empty()) {
      INFO(errors.front());
    }
    REQUIRE(errors.empty());
  }
}

TEST_CASE("compose graph repeated direct stream collect waves remain stable",
          "[core][compose][graph][stream][stress][concurrency][collect]") {
  auto fixture = make_direct_stream_collect_fixture();

  for (int round = 0; round < 8; ++round) {
    auto errors = run_direct_stream_collect_wave(fixture.graph, 8, 32, round * 10000);
    INFO("round=" << round);
    if (!errors.empty()) {
      INFO(errors.front());
    }
    REQUIRE(errors.empty());
  }
}

} // namespace
