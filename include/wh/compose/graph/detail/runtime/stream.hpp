// Defines graph stream emission helpers extracted from graph execution core.
#pragma once

#include <any>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wh/compose/graph/call_options.hpp"
#include "wh/compose/graph/detail/runtime/state.hpp"
#include "wh/compose/runtime/state.hpp"
#include "wh/core/run_context.hpp"

namespace wh::compose::detail::stream_runtime {

inline auto append_debug_event(detail::runtime_state::invoke_outputs &outputs,
                               const graph_call_scope &options,
                               const graph_debug_stream_event &event) -> void {
  if (!has_graph_stream_subscription(options, graph_stream_channel_kind::debug)) {
    return;
  }
  outputs.debug_events.push_back(event);
}

inline auto append_state_snapshot_event(
    detail::runtime_state::invoke_outputs &outputs,
    const graph_state_transition_event &event,
    graph_stream_event_namespace scope) -> void {
  outputs.state_snapshot_events.push_back(graph_state_snapshot_stream_event{
      .scope = std::move(scope),
      .step = event.cause.step,
      .payload = wh::core::any(event),
  });
}

inline auto append_state_delta_event(
    detail::runtime_state::invoke_outputs &outputs,
    const graph_state_transition_event &event,
    graph_stream_event_namespace scope) -> void {
  outputs.state_delta_events.push_back(graph_state_delta_stream_event{
      .scope = std::move(scope),
      .step = event.cause.step,
      .payload = wh::core::any(event),
  });
}

inline auto append_message_event(detail::runtime_state::invoke_outputs &outputs,
                                 graph_stream_event_namespace scope,
                                 const std::size_t step,
                                 const std::string_view message) -> void {
  outputs.message_events.push_back(graph_message_stream_event{
      .scope = std::move(scope),
      .step = step,
      .message = std::string{message},
  });
}

inline auto emit_debug_event(wh::core::run_context &context,
                             detail::runtime_state::invoke_outputs &outputs,
                             const graph_call_scope &options,
                             const graph_debug_stream_event &event,
                             const graph_stream_event_namespace &scope) -> void {
  dispatch_graph_debug_observers(options, event, context);
  append_debug_event(outputs, options, event);
  if (!options.isolate_debug_stream()) {
    append_message_event(outputs, scope, event.step, "debug-decision");
  }
}

inline auto append_custom_events(detail::runtime_state::invoke_outputs &outputs,
                                 const graph_call_scope &options,
                                 const graph_state_transition_event &event,
                                 const graph_stream_event_namespace &scope) -> void {
  for (const auto &subscription : options.options().stream_subscriptions) {
    if (!subscription.enabled ||
        subscription.kind != graph_stream_channel_kind::custom ||
        subscription.custom_channel.empty()) {
      continue;
    }
    outputs.custom_events.push_back(graph_custom_stream_event{
        .scope = scope,
        .step = event.cause.step,
        .channel = subscription.custom_channel,
        .payload = wh::core::any(event),
    });
  }
}

inline auto append_state_transition_events(
    detail::runtime_state::invoke_outputs &outputs,
    const graph_call_scope &options,
    const graph_state_transition_event &event,
    const graph_stream_event_namespace &scope,
    const bool emit_state_snapshot_events,
    const bool emit_state_delta_events, const bool emit_message_events,
    const bool emit_custom_events) -> void {
  if (emit_state_snapshot_events) {
    append_state_snapshot_event(outputs, event, scope);
  }
  if (emit_state_delta_events) {
    append_state_delta_event(outputs, event, scope);
  }
  if (emit_custom_events) {
    append_custom_events(outputs, options, event, scope);
  }
  if (emit_message_events) {
    append_message_event(outputs, scope, event.cause.step, "state-transition");
  }
}

inline auto append_state_transition(graph_transition_log &log,
                                    detail::runtime_state::invoke_outputs &outputs,
                                    const graph_call_scope &options,
                                    const graph_state_transition_event &event,
                                    const graph_stream_event_namespace &scope,
                                    const bool record_log,
                                    const bool emit_state_snapshot_events,
                                    const bool emit_state_delta_events,
                                    const bool emit_message_events,
                                    const bool emit_custom_events)
    -> void {
  if (record_log) {
    log.push_back(event);
    append_state_transition_events(outputs, options, log.back(), scope,
                                   emit_state_snapshot_events,
                                   emit_state_delta_events,
                                   emit_message_events, emit_custom_events);
    return;
  }
  append_state_transition_events(outputs, options, event, scope,
                                 emit_state_snapshot_events,
                                 emit_state_delta_events, emit_message_events,
                                 emit_custom_events);
}

inline auto append_state_transition(graph_transition_log &log,
                                    detail::runtime_state::invoke_outputs &outputs,
                                    const graph_call_scope &options,
                                    graph_state_transition_event &&event,
                                    const graph_stream_event_namespace &scope,
                                    const bool record_log,
                                    const bool emit_state_snapshot_events,
                                    const bool emit_state_delta_events,
                                    const bool emit_message_events,
                                    const bool emit_custom_events)
    -> void {
  if (record_log) {
    log.push_back(std::move(event));
    append_state_transition_events(outputs, options, log.back(), scope,
                                   emit_state_snapshot_events,
                                   emit_state_delta_events,
                                   emit_message_events, emit_custom_events);
    return;
  }
  append_state_transition_events(outputs, options, event, scope,
                                 emit_state_snapshot_events,
                                 emit_state_delta_events, emit_message_events,
                                 emit_custom_events);
}

} // namespace wh::compose::detail::stream_runtime
