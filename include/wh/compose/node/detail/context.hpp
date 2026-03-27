// Defines graph-runtime helpers that project resolved observation/trace state
// into component requests and callback contexts.
#pragma once

#include <optional>
#include <type_traits>
#include <utility>

#include "wh/compose/graph/call_options.hpp"
#include "wh/compose/node/execution.hpp"
#include "wh/core/run_context.hpp"
#include "wh/core/stdexec.hpp"

namespace wh::compose {

namespace detail {

template <typename request_t>
concept component_request_with_options =
    requires(request_t &request, const request_t &const_request) {
      { request.options.component_options() }
          -> std::same_as<wh::core::component_options &>;
      { const_request.options.component_options() }
          -> std::same_as<const wh::core::component_options &>;
    };

[[nodiscard]] inline auto default_node_observation() noexcept
    -> const graph_resolved_node_observation & {
  static const graph_resolved_node_observation observation{};
  return observation;
}

[[nodiscard]] inline auto default_node_trace() noexcept
    -> const graph_node_trace & {
  static const graph_node_trace trace{};
  return trace;
}

[[nodiscard]] inline auto default_node_call_options() noexcept
    -> const graph_call_scope & {
  static const graph_call_options options{};
  static const graph_call_scope scope{options};
  return scope;
}

[[nodiscard]] inline auto make_callback_metadata(
    const graph_node_trace &trace) -> wh::core::callback_run_metadata {
  wh::core::callback_run_metadata metadata{};
  if (!trace.trace_id.empty()) {
    metadata.trace_id = std::string{trace.trace_id};
  }
  metadata.span_id = trace.span_id;
  metadata.parent_span_id = trace.parent_span_id;
  if (trace.path != nullptr) {
    metadata.node_path = *trace.path;
  }
  return metadata;
}

enum class node_callback_binding {
  none = 0,
  projected,
};

enum class node_execution_binding {
  shared = 0,
  forked,
};

struct resolved_node_context_binding {
  node_callback_binding callback{node_callback_binding::none};
  node_execution_binding execution{node_execution_binding::shared};

  [[nodiscard]] auto projects_callbacks() const noexcept -> bool {
    return callback == node_callback_binding::projected;
  }

  [[nodiscard]] auto forks_execution() const noexcept -> bool {
    return execution == node_execution_binding::forked;
  }
};

[[nodiscard]] inline auto resolve_node_context_binding(
    const wh::core::run_context &parent,
    const graph_resolved_node_observation &observation,
    const graph_node_trace &trace) noexcept -> resolved_node_context_binding {
  resolved_node_context_binding binding{};
  if (observation.callbacks_enabled &&
      (parent.callbacks.has_value() || !observation.local_callbacks.empty())) {
    binding.callback = node_callback_binding::projected;
  }

  if (!observation.callbacks_enabled) {
    if (parent.callbacks.has_value()) {
      binding.execution = node_execution_binding::forked;
    }
    return binding;
  }

  if (!observation.local_callbacks.empty()) {
    binding.execution = node_execution_binding::forked;
    return binding;
  }

  if (parent.callbacks.has_value() && !make_callback_metadata(trace).empty()) {
    binding.execution = node_execution_binding::forked;
  }
  return binding;
}

inline auto apply_node_callbacks(
    wh::core::run_context &context,
    const graph_resolved_node_observation &observation,
    const graph_node_trace &trace) -> void {
  if (!observation.callbacks_enabled) {
    context.callbacks.reset();
    return;
  }
  if (context.callbacks.has_value() || !observation.local_callbacks.empty()) {
    if (!context.callbacks.has_value()) {
      context.callbacks.emplace();
    }
    for (const auto &registration : observation.local_callbacks) {
      context.callbacks->manager.register_local_callbacks(registration.config,
                                                          registration.callbacks);
    }
    auto metadata = make_callback_metadata(trace);
    if (!metadata.empty()) {
      context.callbacks->metadata = std::move(metadata);
    }
  }
}

} // namespace detail

[[nodiscard]] inline auto node_observation(const node_runtime &runtime) noexcept
    -> const graph_resolved_node_observation & {
  if (runtime.observation != nullptr) {
    return *runtime.observation;
  }
  return detail::default_node_observation();
}

[[nodiscard]] inline auto node_trace(const node_runtime &runtime) noexcept
    -> const graph_node_trace & {
  if (runtime.trace != nullptr) {
    return *runtime.trace;
  }
  return detail::default_node_trace();
}

[[nodiscard]] inline auto node_call_options(const node_runtime &runtime) noexcept
    -> const graph_call_scope & {
  if (runtime.call_options != nullptr) {
    return *runtime.call_options;
  }
  return detail::default_node_call_options();
}

template <detail::component_request_with_options request_t>
inline auto patch_component_request(
    request_t &request, const graph_resolved_node_observation &observation,
    const graph_node_trace &trace) -> void {
  wh::core::component_override_options overrides{};
  overrides.callbacks_enabled = observation.callbacks_enabled;
  if (!trace.trace_id.empty()) {
    overrides.trace_id = std::string{trace.trace_id};
  }
  if (!trace.span_id.empty()) {
    overrides.span_id = trace.span_id;
  }
  request.options.component_options().set_call_override(std::move(overrides));
}

[[nodiscard]] inline auto make_node_callback_context(
    const wh::core::run_context &parent,
    const graph_resolved_node_observation &observation,
    const graph_node_trace &trace) -> std::optional<wh::core::run_context> {
  const auto binding =
      detail::resolve_node_context_binding(parent, observation, trace);
  if (!binding.projects_callbacks()) {
    return std::nullopt;
  }

  wh::core::run_context callback_context{};
  if (parent.callbacks.has_value()) {
    callback_context.callbacks = parent.callbacks;
  }
  detail::apply_node_callbacks(callback_context, observation, trace);
  return callback_context;
}

[[nodiscard]] inline auto make_node_context(
    const wh::core::run_context &parent,
    const graph_resolved_node_observation &observation,
    const graph_node_trace &trace) -> std::optional<wh::core::run_context> {
  const auto binding =
      detail::resolve_node_context_binding(parent, observation, trace);
  if (!binding.forks_execution()) {
    return std::nullopt;
  }

  auto node_context = wh::core::run_context{parent};
  detail::apply_node_callbacks(node_context, observation, trace);
  return node_context;
}

template <typename invoke_t>
[[nodiscard]] inline auto with_node_context(wh::core::run_context &context,
                                            const node_runtime &runtime,
                                            invoke_t &&invoke)
    -> decltype(auto) {
  const auto &observation = node_observation(runtime);
  const auto &trace = node_trace(runtime);
  const auto binding =
      detail::resolve_node_context_binding(context, observation, trace);
  std::optional<wh::core::run_context> owned_context{};
  if (binding.forks_execution()) {
    owned_context = wh::core::run_context{context};
    detail::apply_node_callbacks(*owned_context, observation, trace);
  }
  auto &node_context = owned_context.has_value() ? *owned_context : context;
  return std::forward<invoke_t>(invoke)(node_context);
}

template <typename invoke_t>
[[nodiscard]] inline auto with_node_call(wh::core::run_context &context,
                                         const node_runtime &runtime,
                                         invoke_t &&invoke) -> decltype(auto) {
  return with_node_context(
      context, runtime,
      [&](wh::core::run_context &node_context) -> decltype(auto) {
        return std::forward<invoke_t>(invoke)(node_context,
                                              node_call_options(runtime));
      });
}

template <typename sender_factory_t>
[[nodiscard]] inline auto bind_node_sender(
    wh::core::run_context &context, const node_runtime &runtime,
    sender_factory_t &&make_sender) -> graph_sender {
  const auto &observation = node_observation(runtime);
  const auto &trace = node_trace(runtime);
  const auto binding =
      detail::resolve_node_context_binding(context, observation, trace);
  std::optional<wh::core::run_context> owned_context{};
  if (binding.forks_execution()) {
    owned_context.emplace(context);
    detail::apply_node_callbacks(*owned_context, observation, trace);
  }
  return ::wh::compose::detail::bridge_graph_sender(
      ::wh::core::detail::defer_sender(
          [owned_context = std::move(owned_context), &context,
           make_sender = std::forward<sender_factory_t>(make_sender)]() mutable {
            auto &node_context =
                owned_context.has_value() ? *owned_context : context;
            return make_sender(node_context);
          }));
}

template <typename sender_factory_t>
[[nodiscard]] inline auto bind_node_call_sender(
    wh::core::run_context &context, const node_runtime &runtime,
    sender_factory_t &&make_sender) -> graph_sender {
  const auto *call_options = std::addressof(node_call_options(runtime));
  return bind_node_sender(
      context, runtime,
      [call_options,
       make_sender = std::forward<sender_factory_t>(make_sender)](
          wh::core::run_context &node_context) mutable -> graph_sender {
        return make_sender(node_context, *call_options);
      });
}

[[nodiscard]] inline auto own_node_input(graph_value &input) -> graph_value {
  if (input.copyable()) {
    return graph_value{input};
  }
  return std::move(input);
}

namespace detail {

template <typename sender_factory_t, typename input_t>
[[nodiscard]] inline auto invoke_input_sender(
    sender_factory_t &make_sender, input_t &&input,
    wh::core::run_context &context,
    const graph_call_scope &call_options) -> graph_sender {
  if constexpr (requires {
                  make_sender(std::forward<input_t>(input), context,
                              call_options);
                }) {
    return ::wh::compose::detail::bridge_graph_sender(
        make_sender(std::forward<input_t>(input), context, call_options));
  } else {
    return ::wh::compose::detail::bridge_graph_sender(
        make_sender(std::forward<input_t>(input), context));
  }
}

} // namespace detail

template <typename sender_factory_t>
[[nodiscard]] inline auto bind_value_sender(
    graph_value &input, wh::core::run_context &context,
    const node_runtime &runtime, sender_factory_t &&make_sender)
    -> graph_sender {
  auto owned_input = own_node_input(input);
  return bind_node_call_sender(
      context, runtime,
      [input = std::move(owned_input),
       make_sender = std::forward<sender_factory_t>(make_sender)](
          wh::core::run_context &callback_context,
          const graph_call_scope &call_options) mutable -> graph_sender {
        return detail::invoke_input_sender(make_sender, input, callback_context,
                                           call_options);
      });
}

template <typename sender_factory_t>
[[nodiscard]] inline auto bind_reader_sender(
    graph_value &input, wh::core::run_context &context,
    const node_runtime &runtime, sender_factory_t &&make_sender)
    -> graph_sender {
  auto *reader = wh::core::any_cast<graph_stream_reader>(&input);
  if (reader == nullptr) {
    return ::wh::compose::detail::failure_graph_sender(
        wh::core::errc::type_mismatch);
  }
  auto owned_input = std::move(*reader);
  return bind_node_call_sender(
      context, runtime,
      [input = std::move(owned_input),
       make_sender = std::forward<sender_factory_t>(make_sender)](
          wh::core::run_context &callback_context,
          const graph_call_scope &call_options) mutable -> graph_sender {
        return detail::invoke_input_sender(make_sender, std::move(input),
                                           callback_context, call_options);
      });
}

} // namespace wh::compose
