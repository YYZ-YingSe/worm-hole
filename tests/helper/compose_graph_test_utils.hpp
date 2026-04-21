#pragma once

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/compose/graph/graph.hpp"
#include "wh/compose/graph/stream.hpp"
#include "wh/compose/node.hpp"
#include "wh/compose/node/detail/runtime_access.hpp"
#include "wh/compose/node/lambda.hpp"
#include "wh/core/stdexec/resume_scheduler.hpp"

namespace wh::testing::helper {

namespace detail {

struct nested_graph_test_state {
  [[nodiscard]] static auto scheduler() noexcept
      -> const wh::core::detail::any_resume_scheduler_t & {
    static const auto inline_graph_scheduler =
        wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{});
    return inline_graph_scheduler;
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

inline auto ensure_nested_test_runtime(wh::compose::node_runtime &runtime,
                                       std::shared_ptr<nested_graph_test_state> &nested_state)
    -> void {
  if (!wh::compose::detail::node_runtime_access::nested_entry(runtime).bound()) {
    nested_state = std::make_shared<nested_graph_test_state>();
    wh::compose::detail::node_runtime_access::bind_internal(
        runtime, wh::compose::detail::node_runtime_access::invoke_outputs(runtime),
        wh::compose::nested_graph_entry{
            .state = nested_state.get(),
            .start = &nested_graph_test_state::invoke,
        });
  }
  if (runtime.control_scheduler() == nullptr) {
    runtime.set_control_scheduler(std::addressof(nested_graph_test_state::scheduler()));
  }
}

} // namespace detail

template <typename result_t, stdexec::sender sender_t>
[[nodiscard]] inline auto wait_sender_result(sender_t sender) -> result_t {
  auto waited = stdexec::sync_wait(std::move(sender));
  if (!waited.has_value()) {
    return result_t::failure(wh::core::errc::canceled);
  }
  return std::move(std::get<0>(waited.value()));
}

[[nodiscard]] inline auto test_graph_scheduler() noexcept
    -> const wh::core::detail::any_resume_scheduler_t & {
  return detail::nested_graph_test_state::scheduler();
}

[[nodiscard]] inline auto make_graph_request(wh::compose::graph_value input)
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
[[nodiscard]] inline auto make_graph_request(input_t &&input) -> wh::compose::graph_invoke_request {
  wh::compose::graph_invoke_request request{};
  request.input = wh::compose::graph_input::value(std::forward<input_t>(input));
  return request;
}

[[nodiscard]] inline auto make_graph_request(wh::compose::graph_input input)
    -> wh::compose::graph_invoke_request {
  wh::compose::graph_invoke_request request{};
  request.input = std::move(input);
  return request;
}

[[nodiscard]] inline auto make_graph_request(wh::compose::graph_stream_reader input)
    -> wh::compose::graph_invoke_request {
  wh::compose::graph_invoke_request request{};
  request.input = wh::compose::graph_input::stream(std::move(input));
  return request;
}

template <typename input_t>
[[nodiscard]] inline auto make_graph_request(input_t &&input,
                                             const wh::compose::graph_call_options &options)
    -> wh::compose::graph_invoke_request {
  auto request = make_graph_request(std::forward<input_t>(input));
  request.controls.call = options;
  return request;
}

template <typename input_t>
[[nodiscard]] inline auto
make_graph_request(input_t &&input, wh::compose::graph_invoke_controls controls,
                   const wh::compose::graph_runtime_services *services = nullptr)
    -> wh::compose::graph_invoke_request {
  auto request = make_graph_request(std::forward<input_t>(input));
  request.controls = std::move(controls);
  request.services = services;
  return request;
}

template <typename input_t>
[[nodiscard]] inline auto
make_graph_request(input_t &&input, const wh::compose::graph_call_options &options,
                   wh::compose::graph_invoke_controls controls,
                   const wh::compose::graph_runtime_services *services = nullptr)
    -> wh::compose::graph_invoke_request {
  auto request = make_graph_request(std::forward<input_t>(input), std::move(controls), services);
  request.controls.call = options;
  return request;
}

namespace detail {

inline auto mutable_checkpoint_pending_inputs(wh::compose::checkpoint_state &state)
    -> wh::compose::checkpoint_pending_inputs & {
  if (state.runtime.dag.has_value() && !state.runtime.pregel.has_value()) {
    return state.runtime.dag->pending_inputs;
  }
  if (state.runtime.pregel.has_value() && !state.runtime.dag.has_value()) {
    return state.runtime.pregel->pending_inputs;
  }
  if (state.runtime.dag.has_value() && state.runtime.pregel.has_value()) {
    return state.restore_shape.options.mode == wh::compose::graph_runtime_mode::pregel
               ? state.runtime.pregel->pending_inputs
               : state.runtime.dag->pending_inputs;
  }
  if (state.restore_shape.options.mode == wh::compose::graph_runtime_mode::pregel) {
    return state.runtime.pregel.has_value() ? state.runtime.pregel->pending_inputs
                                            : state.runtime.pregel.emplace().pending_inputs;
  }
  return state.runtime.dag.has_value() ? state.runtime.dag->pending_inputs
                                       : state.runtime.dag.emplace().pending_inputs;
}

[[nodiscard]] inline auto find_checkpoint_pending_inputs(const wh::compose::checkpoint_state &state)
    -> const wh::compose::checkpoint_pending_inputs * {
  if (state.runtime.dag.has_value() && !state.runtime.pregel.has_value()) {
    return std::addressof(state.runtime.dag->pending_inputs);
  }
  if (state.runtime.pregel.has_value() && !state.runtime.dag.has_value()) {
    return std::addressof(state.runtime.pregel->pending_inputs);
  }
  if (state.runtime.dag.has_value() && state.runtime.pregel.has_value()) {
    return state.restore_shape.options.mode == wh::compose::graph_runtime_mode::pregel
               ? std::addressof(state.runtime.pregel->pending_inputs)
               : std::addressof(state.runtime.dag->pending_inputs);
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
}

} // namespace detail

inline auto set_checkpoint_entry_input(wh::compose::checkpoint_state &state, const int value)
    -> void {
  detail::mutable_checkpoint_pending_inputs(state).entry = wh::core::any(value);
}

inline auto set_checkpoint_entry_input(wh::compose::checkpoint_state &state,
                                       wh::compose::graph_value value) -> void {
  detail::mutable_checkpoint_pending_inputs(state).entry = std::move(value);
}

[[nodiscard]] inline auto find_checkpoint_entry_input(const wh::compose::checkpoint_state &state)
    -> const wh::compose::graph_value * {
  const auto *pending = detail::find_checkpoint_pending_inputs(state);
  if (pending == nullptr || !pending->entry.has_value()) {
    return nullptr;
  }
  return std::addressof(*pending->entry);
}

[[nodiscard]] inline auto checkpoint_entry_input(const wh::compose::checkpoint_state &state)
    -> wh::core::result<int> {
  const auto *payload = find_checkpoint_entry_input(state);
  if (payload == nullptr) {
    return wh::core::result<int>::failure(wh::core::errc::not_found);
  }
  if (const auto *typed = wh::core::any_cast<int>(payload); typed != nullptr) {
    return *typed;
  }
  return wh::core::result<int>::failure(wh::core::errc::type_mismatch);
}

inline auto set_checkpoint_node_input(wh::compose::checkpoint_state &state, std::string key,
                                      wh::compose::graph_value value,
                                      const std::uint32_t node_id = 0U) -> void {
  auto &pending = detail::mutable_checkpoint_pending_inputs(state);
  for (auto &node_input : pending.nodes) {
    if (node_input.key == key) {
      node_input.node_id = node_id;
      node_input.input = std::move(value);
      return;
    }
  }
  pending.nodes.push_back(wh::compose::checkpoint_node_input{
      .node_id = node_id,
      .key = std::move(key),
      .input = std::move(value),
  });
}

[[nodiscard]] inline auto find_checkpoint_node_input(const wh::compose::checkpoint_state &state,
                                                     const std::string_view key)
    -> const wh::compose::graph_value * {
  const auto *pending = detail::find_checkpoint_pending_inputs(state);
  if (pending == nullptr) {
    return nullptr;
  }
  for (const auto &node_input : pending->nodes) {
    if (node_input.key == key) {
      return std::addressof(node_input.input);
    }
  }
  return nullptr;
}

[[nodiscard]] inline auto
invoke_value_sync(wh::compose::graph &graph, wh::compose::graph_value input,
                  wh::core::run_context &context, wh::compose::graph_call_options call_options,
                  const wh::compose::graph_runtime_services *services,
                  wh::compose::graph_invoke_controls controls)
    -> wh::core::result<wh::compose::graph_value>;

[[nodiscard]] inline auto add_test_node(wh::compose::graph &graph,
                                        const wh::compose::component_node &node)
    -> wh::core::result<void> {
  return graph.add_component(node);
}

[[nodiscard]] inline auto add_test_node(wh::compose::graph &graph,
                                        const wh::compose::lambda_node &node)
    -> wh::core::result<void> {
  return graph.add_lambda(node);
}

[[nodiscard]] inline auto add_test_node(wh::compose::graph &graph,
                                        const wh::compose::subgraph_node &node)
    -> wh::core::result<void> {
  return graph.add_subgraph(node);
}

[[nodiscard]] inline auto add_test_node(wh::compose::graph &graph,
                                        const wh::compose::tools_node &node)
    -> wh::core::result<void> {
  return graph.add_tools(node);
}

[[nodiscard]] inline auto add_test_node(wh::compose::graph &graph,
                                        const wh::compose::passthrough_node &node)
    -> wh::core::result<void> {
  return graph.add_passthrough(node);
}

struct compiled_single_node_graph {
  std::shared_ptr<wh::compose::graph> graph{};
  const wh::compose::compiled_node *node{nullptr};
};

[[nodiscard]] inline auto
make_test_node_runtime(const wh::compose::graph_call_scope *call_options = nullptr,
                       const std::size_t parallel_gate = 0U) -> wh::compose::node_runtime {
  wh::compose::node_runtime runtime{};
  runtime.set_parallel_gate(parallel_gate);
  runtime.set_call_options(call_options);
  return runtime;
}

template <typename node_t>
[[nodiscard]] inline auto build_single_node_graph(const node_t &node)
    -> wh::core::result<compiled_single_node_graph> {
  auto graph = std::make_shared<wh::compose::graph>();
  auto added = add_test_node(*graph, node);
  if (added.has_error()) {
    return wh::core::result<compiled_single_node_graph>::failure(added.error());
  }
  auto entry = graph->add_entry_edge(std::string{node.key()});
  if (entry.has_error()) {
    return wh::core::result<compiled_single_node_graph>::failure(entry.error());
  }
  auto exit = graph->add_exit_edge(std::string{node.key()});
  if (exit.has_error()) {
    return wh::core::result<compiled_single_node_graph>::failure(exit.error());
  }
  auto compiled = graph->compile();
  if (compiled.has_error()) {
    return wh::core::result<compiled_single_node_graph>::failure(compiled.error());
  }
  auto compiled_node = graph->compiled_node_by_key(node.key());
  if (compiled_node.has_error()) {
    return wh::core::result<compiled_single_node_graph>::failure(compiled_node.error());
  }
  return compiled_single_node_graph{
      .graph = std::move(graph),
      .node = std::addressof(compiled_node.value().get()),
  };
}

template <typename node_t>
[[nodiscard]] inline auto
invoke_single_node_graph(const node_t &node, wh::compose::graph_value input,
                         wh::core::run_context &context,
                         wh::compose::graph_call_options call_options = {},
                         const wh::compose::graph_runtime_services *services = nullptr,
                         wh::compose::graph_invoke_controls controls = {})
    -> wh::core::result<wh::compose::graph_value> {
  auto built = build_single_node_graph(node);
  if (built.has_error()) {
    return wh::core::result<wh::compose::graph_value>::failure(built.error());
  }
  return invoke_value_sync(*built->graph, std::move(input), context, std::move(call_options),
                           services, std::move(controls));
}

template <typename node_t>
[[nodiscard]] inline auto
execute_single_compiled_node(const node_t &node, wh::compose::graph_value input,
                             wh::core::run_context &context, wh::compose::node_runtime runtime = {})
    -> wh::core::result<wh::compose::graph_value> {
  auto built = build_single_node_graph(node);
  if (built.has_error()) {
    return wh::core::result<wh::compose::graph_value>::failure(built.error());
  }
  auto nested_state = std::shared_ptr<detail::nested_graph_test_state>{};
  detail::ensure_nested_test_runtime(runtime, nested_state);
  if (wh::compose::compiled_node_is_sync(*built->node)) {
    return wh::compose::run_compiled_sync_node(*built->node, input, context, runtime);
  }
  auto waited = stdexec::sync_wait(
      wh::compose::run_compiled_async_node(*built->node, input, context, runtime));
  if (!waited.has_value()) {
    return wh::core::result<wh::compose::graph_value>::failure(wh::core::errc::canceled);
  }
  return std::get<0>(std::move(*waited));
}

template <typename value_t>
[[nodiscard]] inline auto read_graph_value(const wh::compose::graph_value &value)
    -> wh::core::result<value_t> {
  if (const auto *typed = wh::core::any_cast<value_t>(&value); typed != nullptr) {
    return *typed;
  }
  return wh::core::result<value_t>::failure(wh::core::errc::type_mismatch);
}

template <typename value_t>
[[nodiscard]] inline auto read_graph_value(wh::compose::graph_value &&value)
    -> wh::core::result<value_t> {
  if (auto *typed = wh::core::any_cast<value_t>(&value); typed != nullptr) {
    return std::move(*typed);
  }
  return wh::core::result<value_t>::failure(wh::core::errc::type_mismatch);
}

template <typename value_t>
[[nodiscard]] inline auto read_graph_value_cref(const wh::compose::graph_value &value)
    -> wh::core::result<std::reference_wrapper<const value_t>> {
  if (const auto *typed = wh::core::any_cast<value_t>(&value); typed != nullptr) {
    return std::cref(*typed);
  }
  return wh::core::result<std::reference_wrapper<const value_t>>::failure(
      wh::core::errc::type_mismatch);
}

[[nodiscard]] inline auto
invoke_graph_sync(wh::compose::graph &graph, wh::compose::graph_value input,
                  wh::core::run_context &context, wh::compose::graph_call_options call_options = {},
                  const wh::compose::graph_runtime_services *services = nullptr,
                  wh::compose::graph_invoke_controls controls = {})
    -> wh::core::result<wh::compose::graph_invoke_result> {
  controls.call = std::move(call_options);
  auto request = make_graph_request(std::move(input), std::move(controls), services);

  auto waited = stdexec::sync_wait(graph.invoke(context, std::move(request)));
  if (!waited.has_value()) {
    return wh::core::result<wh::compose::graph_invoke_result>::failure(wh::core::errc::canceled);
  }
  return std::get<0>(std::move(*waited));
}

[[nodiscard]] inline auto
invoke_graph_sync(wh::compose::graph &graph, wh::compose::graph_input input,
                  wh::core::run_context &context, wh::compose::graph_call_options call_options = {},
                  const wh::compose::graph_runtime_services *services = nullptr,
                  wh::compose::graph_invoke_controls controls = {})
    -> wh::core::result<wh::compose::graph_invoke_result> {
  controls.call = std::move(call_options);
  auto request = make_graph_request(std::move(input));
  request.controls = std::move(controls);
  request.services = services;

  auto waited = stdexec::sync_wait(graph.invoke(context, std::move(request)));
  if (!waited.has_value()) {
    return wh::core::result<wh::compose::graph_invoke_result>::failure(wh::core::errc::canceled);
  }
  return std::get<0>(std::move(*waited));
}

template <typename invokable_t>
concept graph_request_invokable = requires(invokable_t &invokable, wh::core::run_context &context,
                                           wh::compose::graph_invoke_request request) {
  invokable.invoke(context, std::move(request));
};

[[nodiscard]] inline auto
invoke_graph_sync(wh::compose::graph &graph, wh::compose::graph_value input,
                  wh::core::run_context &context, wh::compose::graph_invoke_controls controls,
                  const wh::compose::graph_runtime_services *services = nullptr)
    -> wh::core::result<wh::compose::graph_invoke_result> {
  return invoke_graph_sync(graph, std::move(input), context, {}, services, std::move(controls));
}

[[nodiscard]] inline auto
invoke_graph_sync(wh::compose::graph &graph, wh::compose::graph_input input,
                  wh::core::run_context &context, wh::compose::graph_invoke_controls controls,
                  const wh::compose::graph_runtime_services *services = nullptr)
    -> wh::core::result<wh::compose::graph_invoke_result> {
  return invoke_graph_sync(graph, std::move(input), context, {}, services, std::move(controls));
}

template <typename invokable_t>
  requires graph_request_invokable<invokable_t>
[[nodiscard]] inline auto
invoke_graph_sync(invokable_t &invokable, wh::compose::graph_value input,
                  wh::core::run_context &context, wh::compose::graph_call_options call_options = {},
                  const wh::compose::graph_runtime_services *services = nullptr,
                  wh::compose::graph_invoke_controls controls = {})
    -> wh::core::result<wh::compose::graph_invoke_result> {
  controls.call = std::move(call_options);
  auto waited = stdexec::sync_wait(invokable.invoke(
      context, make_graph_request(std::move(input), std::move(controls), services)));
  if (!waited.has_value()) {
    return wh::core::result<wh::compose::graph_invoke_result>::failure(wh::core::errc::canceled);
  }
  return std::get<0>(std::move(*waited));
}

template <typename invokable_t>
  requires graph_request_invokable<invokable_t>
[[nodiscard]] inline auto
invoke_graph_sync(invokable_t &invokable, wh::compose::graph_value input,
                  wh::core::run_context &context, wh::compose::graph_invoke_controls controls,
                  const wh::compose::graph_runtime_services *services = nullptr)
    -> wh::core::result<wh::compose::graph_invoke_result> {
  return invoke_graph_sync(invokable, std::move(input), context, {}, services, std::move(controls));
}

[[nodiscard]] inline auto
invoke_value_sync(wh::compose::graph &graph, wh::compose::graph_value input,
                  wh::core::run_context &context, wh::compose::graph_call_options call_options = {},
                  const wh::compose::graph_runtime_services *services = nullptr,
                  wh::compose::graph_invoke_controls controls = {})
    -> wh::core::result<wh::compose::graph_value> {
  auto invoked = invoke_graph_sync(graph, std::move(input), context, std::move(call_options),
                                   services, std::move(controls));
  if (invoked.has_error()) {
    return wh::core::result<wh::compose::graph_value>::failure(invoked.error());
  }
  if (invoked->output_status.has_error()) {
    return wh::core::result<wh::compose::graph_value>::failure(invoked->output_status.error());
  }
  return std::move(invoked->output_status).value();
}

template <typename invokable_t>
  requires graph_request_invokable<invokable_t>
[[nodiscard]] inline auto
invoke_value_sync(invokable_t &invokable, wh::compose::graph_value input,
                  wh::core::run_context &context, wh::compose::graph_call_options call_options = {},
                  const wh::compose::graph_runtime_services *services = nullptr,
                  wh::compose::graph_invoke_controls controls = {})
    -> wh::core::result<wh::compose::graph_value> {
  auto invoked = invoke_graph_sync(invokable, std::move(input), context, std::move(call_options),
                                   services, std::move(controls));
  if (invoked.has_error()) {
    return wh::core::result<wh::compose::graph_value>::failure(invoked.error());
  }
  if (invoked->output_status.has_error()) {
    return wh::core::result<wh::compose::graph_value>::failure(invoked->output_status.error());
  }
  return std::move(invoked->output_status).value();
}

[[nodiscard]] inline auto make_int_add_node(const std::string_view key, const int delta) {
  return wh::compose::make_lambda_node(
      std::string{key},
      [delta](const wh::compose::graph_value &input, wh::core::run_context &,
              const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
        auto typed = read_graph_value<int>(input);
        if (typed.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(typed.error());
        }
        return wh::compose::graph_value{typed.value() + delta};
      });
}

[[nodiscard]] inline auto make_int_mul_node(const std::string_view key, const int factor) {
  return wh::compose::make_lambda_node(
      std::string{key},
      [factor](
          const wh::compose::graph_value &input, wh::core::run_context &,
          const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
        auto typed = read_graph_value<int>(input);
        if (typed.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(typed.error());
        }
        return wh::compose::graph_value{typed.value() * factor};
      });
}

[[nodiscard]] inline auto make_auto_contract_edge_options() -> wh::compose::edge_options {
  return {};
}

[[nodiscard]] inline auto make_int_graph_stream(std::initializer_list<int> values,
                                                const std::size_t capacity = 16U)
    -> wh::core::result<wh::compose::graph_stream_reader> {
  auto [writer, reader] = wh::compose::make_graph_stream(capacity);
  for (const auto value : values) {
    auto pushed = writer.try_write(wh::core::any(value));
    if (pushed.has_error()) {
      return wh::core::result<wh::compose::graph_stream_reader>::failure(pushed.error());
    }
  }
  auto closed = writer.close();
  if (closed.has_error()) {
    return wh::core::result<wh::compose::graph_stream_reader>::failure(closed.error());
  }
  return std::move(reader);
}

[[nodiscard]] inline auto make_tool_batch(std::initializer_list<wh::compose::tool_call> calls)
    -> wh::compose::tool_batch {
  return wh::compose::tool_batch{
      .calls = std::vector<wh::compose::tool_call>{calls},
  };
}

[[nodiscard]] inline auto collect_tool_results(const wh::compose::graph_value &value)
    -> wh::core::result<std::reference_wrapper<const std::vector<wh::compose::tool_result>>> {
  return read_graph_value_cref<std::vector<wh::compose::tool_result>>(value);
}

[[nodiscard]] inline auto collect_tool_events(wh::compose::graph_stream_reader reader)
    -> wh::core::result<std::vector<wh::compose::tool_event>> {
  auto chunks = wh::compose::collect_graph_stream_reader(std::move(reader));
  if (chunks.has_error()) {
    return wh::core::result<std::vector<wh::compose::tool_event>>::failure(chunks.error());
  }

  std::vector<wh::compose::tool_event> events{};
  events.reserve(chunks.value().size());
  for (auto &chunk : chunks.value()) {
    auto event = read_graph_value<wh::compose::tool_event>(std::move(chunk));
    if (event.has_error()) {
      return wh::core::result<std::vector<wh::compose::tool_event>>::failure(event.error());
    }
    events.push_back(std::move(event).value());
  }
  return events;
}

[[nodiscard]] inline auto collect_int_graph_stream(wh::compose::graph_stream_reader reader)
    -> wh::core::result<std::vector<int>> {
  auto chunks = wh::compose::collect_graph_stream_reader(std::move(reader));
  if (chunks.has_error()) {
    return wh::core::result<std::vector<int>>::failure(chunks.error());
  }

  std::vector<int> values{};
  values.reserve(chunks.value().size());
  for (auto &chunk : chunks.value()) {
    auto typed = read_graph_value<int>(std::move(chunk));
    if (typed.has_error()) {
      return wh::core::result<std::vector<int>>::failure(typed.error());
    }
    values.push_back(typed.value());
  }
  return values;
}

[[nodiscard]] inline auto collect_int_graph_chunk_values(const wh::compose::graph_value &value)
    -> wh::core::result<std::vector<int>> {
  const auto *chunks = wh::core::any_cast<std::vector<wh::compose::graph_value>>(&value);
  if (chunks == nullptr) {
    return wh::core::result<std::vector<int>>::failure(wh::core::errc::type_mismatch);
  }

  std::vector<int> values{};
  values.reserve(chunks->size());
  for (const auto &chunk : *chunks) {
    auto typed = read_graph_value<int>(chunk);
    if (typed.has_error()) {
      return wh::core::result<std::vector<int>>::failure(typed.error());
    }
    values.push_back(typed.value());
  }
  return values;
}

[[nodiscard]] inline auto collect_string_graph_chunk_values(const wh::compose::graph_value &value)
    -> wh::core::result<std::vector<std::string>> {
  const auto *chunks = wh::core::any_cast<std::vector<wh::compose::graph_value>>(&value);
  if (chunks == nullptr) {
    return wh::core::result<std::vector<std::string>>::failure(wh::core::errc::type_mismatch);
  }

  std::vector<std::string> values{};
  values.reserve(chunks->size());
  for (const auto &chunk : *chunks) {
    auto typed = read_graph_value<std::string>(chunk);
    if (typed.has_error()) {
      return wh::core::result<std::vector<std::string>>::failure(typed.error());
    }
    values.push_back(std::move(typed).value());
  }
  return values;
}

[[nodiscard]] inline auto collect_int_graph_chunks(wh::compose::graph_value &value)
    -> wh::core::result<std::vector<int>> {
  auto forked = wh::compose::detail::fork_graph_reader_payload(value);
  if (forked.has_error()) {
    return wh::core::result<std::vector<int>>::failure(forked.error());
  }
  auto reader = read_graph_value<wh::compose::graph_stream_reader>(std::move(forked).value());
  if (reader.has_error()) {
    return wh::core::result<std::vector<int>>::failure(reader.error());
  }

  std::vector<int> values{};
  for (;;) {
    auto next = reader.value().read();
    if (next.has_error()) {
      return wh::core::result<std::vector<int>>::failure(next.error());
    }
    auto chunk = std::move(next).value();
    if (chunk.is_terminal_eof()) {
      return values;
    }
    if (chunk.error != wh::core::errc::ok || !chunk.value.has_value()) {
      return wh::core::result<std::vector<int>>::failure(
          chunk.error == wh::core::errc::ok ? wh::core::errc::invalid_argument : chunk.error);
    }
    auto typed = read_graph_value<int>(std::move(*chunk.value));
    if (typed.has_error()) {
      return wh::core::result<std::vector<int>>::failure(typed.error());
    }
    values.push_back(typed.value());
  }
}

[[nodiscard]] inline auto collect_string_graph_chunks(wh::compose::graph_value &value)
    -> wh::core::result<std::vector<std::string>> {
  auto forked = wh::compose::detail::fork_graph_reader_payload(value);
  if (forked.has_error()) {
    return wh::core::result<std::vector<std::string>>::failure(forked.error());
  }
  auto reader = read_graph_value<wh::compose::graph_stream_reader>(std::move(forked).value());
  if (reader.has_error()) {
    return wh::core::result<std::vector<std::string>>::failure(reader.error());
  }

  std::vector<std::string> values{};
  for (;;) {
    auto next = reader.value().read();
    if (next.has_error()) {
      return wh::core::result<std::vector<std::string>>::failure(next.error());
    }
    auto chunk = std::move(next).value();
    if (chunk.is_terminal_eof()) {
      return values;
    }
    if (chunk.error != wh::core::errc::ok || !chunk.value.has_value()) {
      return wh::core::result<std::vector<std::string>>::failure(
          chunk.error == wh::core::errc::ok ? wh::core::errc::invalid_argument : chunk.error);
    }
    auto typed = read_graph_value<std::string>(std::move(*chunk.value));
    if (typed.has_error()) {
      return wh::core::result<std::vector<std::string>>::failure(typed.error());
    }
    values.push_back(std::move(typed).value());
  }
}

[[nodiscard]] inline auto sum_ints(const std::vector<int> &values) -> int {
  int total = 0;
  for (const auto value : values) {
    total += value;
  }
  return total;
}

} // namespace wh::testing::helper
