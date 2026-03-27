// Defines typed invoke-time config/runtime state used by compose graph execution.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wh/compose/graph/error.hpp"
#include "wh/compose/graph/call_options.hpp"
#include "wh/compose/node/execution.hpp"
#include "wh/compose/graph/detail/runtime/schedule.hpp"
#include "wh/compose/runtime/interrupt.hpp"
#include "wh/compose/runtime/resume.hpp"
#include "wh/compose/runtime/state.hpp"
#include "wh/compose/reduce/values_merge.hpp"
#include "wh/core/any.hpp"
#include "wh/core/run_context.hpp"
#include "wh/internal/merge.hpp"

namespace wh::compose::detail::runtime_state {

/// Session key carrying last successfully completed node keys on terminal paths.
inline constexpr std::string_view graph_runtime_last_completed_nodes_session_key =
    "compose.graph.runtime.last_completed_nodes";

/// Snapshot of run-scoped external graph configuration resolved once at invoke start.
struct invoke_config {
  /// Optional state-handler registry injected by caller.
  const graph_state_handler_registry *state_handlers{nullptr};
  /// Optional values-merge registry used by fan-in of value outputs.
  const wh::internal::values_merge_registry *values_merge_registry{nullptr};
  /// Optional pre-node interrupt hook.
  graph_interrupt_node_hook interrupt_pre_hook{nullptr};
  /// Optional post-node interrupt hook.
  graph_interrupt_node_hook interrupt_post_hook{nullptr};
  /// Optional explicit interrupt contexts used by resume patching.
  std::vector<wh::core::interrupt_context> resume_contexts{};
  /// Optional interrupt signals forwarded from outer graphs.
  std::vector<wh::core::interrupt_signal> subgraph_interrupt_signals{};
  /// Optional single resume decision patch.
  std::optional<interrupt_resume_decision> resume_decision{};
  /// Optional batch resume patch.
  std::vector<resume_batch_item> batch_resume_items{};
  /// True re-interrupts unmatched contexts after applying resume patches.
  bool reinterrupt_unmatched{true};
  /// Runtime cache scope override inherited from ambient context.
  std::optional<std::string> cache_scope_override{};
  /// Pregel max-step override inherited from ambient context.
  std::optional<std::size_t> pregel_max_steps_override{};
  /// Parallel-branch merge policy for this invoke run.
  graph_branch_merge branch_merge{
      graph_branch_merge::set_union};
};

/// Mutable invoke-owned publishable outputs accumulated during one graph run.
struct invoke_outputs {
  /// True publishes the transition log session artifact for this run.
  bool publish_transition_log{false};
  /// Transition log for state replay/debugging.
  graph_transition_log transition_log{};
  /// Debug stream events accumulated during execution.
  std::vector<graph_debug_stream_event> debug_events{};
  /// State-snapshot stream payloads accumulated during execution.
  std::vector<graph_state_snapshot_stream_event> state_snapshot_events{};
  /// State-delta stream payloads accumulated during execution.
  std::vector<graph_state_delta_stream_event> state_delta_events{};
  /// Message stream payloads accumulated during execution.
  std::vector<graph_message_stream_event> message_events{};
  /// Custom stream payloads accumulated during execution.
  std::vector<graph_custom_stream_event> custom_events{};
  /// Last completed node set published on terminal paths.
  std::vector<std::string> last_completed_nodes{};
  /// Optional step-limit error detail captured for this run.
  std::optional<graph_step_limit_error_detail> step_limit_error{};
  /// Optional node-timeout detail captured for this run.
  std::optional<graph_node_timeout_error_detail> node_timeout_error{};
  /// Optional node-run error detail captured for this run.
  std::optional<graph_node_run_error_detail> node_run_error{};
  /// Optional graph-run error detail captured for this run.
  std::optional<graph_run_error_detail> graph_run_error{};
  /// Optional stream-read error detail captured for this run.
  std::optional<graph_new_stream_read_error_detail> stream_read_error{};
  /// Optional external-interrupt resolution captured for this run.
  std::optional<graph_external_interrupt_resolution_kind>
      external_interrupt_resolution{};
};

/// Invoke-owned trace state resolved once at graph entry.
struct graph_trace_state {
  /// Distributed trace id shared by the whole invoke run.
  std::string trace_id{};
  /// Parent span id for the graph root span.
  std::string parent_span_id{};
  /// Span id generated for the graph root itself.
  std::string graph_span_id{};
  /// Next monotonic sequence used for node span ids.
  std::uint64_t next_span_sequence{1U};
};

/// Per-node typed runtime scope stored on the execution frame.
struct node_scope {
  /// Fully resolved runtime path of the currently executing node.
  node_path path{};
  /// Resolved component options for the currently executing node.
  const graph_component_option_map *component_options{nullptr};
  /// Resolved observation state for the currently executing node.
  const graph_resolved_node_observation *observation{nullptr};
  /// Concrete trace payload for the current execution attempt.
  graph_node_trace trace{};
  /// Node-local process-state object visible only to this node run.
  graph_process_state *local_process_state{nullptr};
};

inline auto clear_published_outputs(wh::core::run_context &context) -> void {
  context.session_values.erase(std::string{graph_transition_log_session_key});
  context.session_values.erase(std::string{graph_runtime_last_completed_nodes_session_key});
  context.session_values.erase(std::string{graph_debug_stream_session_key});
  context.session_values.erase(std::string{graph_state_snapshot_stream_session_key});
  context.session_values.erase(std::string{graph_state_delta_stream_session_key});
  context.session_values.erase(std::string{graph_message_stream_session_key});
  context.session_values.erase(std::string{graph_custom_stream_session_key});
  context.session_values.erase(std::string{graph_step_limit_error_session_key});
  context.session_values.erase(std::string{graph_node_timeout_error_session_key});
  context.session_values.erase(std::string{graph_node_run_error_session_key});
  context.session_values.erase(std::string{graph_run_error_session_key});
  context.session_values.erase(std::string{graph_new_stream_read_error_session_key});
  context.session_values.erase(
      std::string{graph_external_interrupt_resolution_session_key});
}

template <typename value_t>
[[nodiscard]] inline auto read_optional_context_value(
    const wh::core::run_context &context, const std::string_view key)
    -> wh::core::result<const value_t *> {
  const auto iter = context.session_values.find(key);
  if (iter == context.session_values.end()) {
    return static_cast<const value_t *>(nullptr);
  }
  const auto *typed = wh::core::any_cast<value_t>(&iter->second);
  if (typed == nullptr) {
    return wh::core::result<const value_t *>::failure(
        wh::core::errc::type_mismatch);
  }
  return typed;
}

[[nodiscard]] inline auto snapshot_invoke_config(
    const wh::core::run_context &context) -> wh::core::result<invoke_config> {
  invoke_config config{};

  auto state_handlers =
      read_optional_context_value<graph_state_handler_registry>(
          context, graph_state_handlers_session_key);
  if (state_handlers.has_error()) {
    return wh::core::result<invoke_config>::failure(state_handlers.error());
  }
  config.state_handlers = state_handlers.value();

  auto values_registry =
      read_optional_context_value<const wh::internal::values_merge_registry *>(
          context, graph_values_merge_registry_session_key);
  if (values_registry.has_error()) {
    return wh::core::result<invoke_config>::failure(values_registry.error());
  }
  if (values_registry.value() != nullptr) {
    config.values_merge_registry = *values_registry.value();
  }

  auto interrupt_pre = read_optional_context_value<graph_interrupt_node_hook>(
      context, graph_interrupt_pre_hook_session_key);
  if (interrupt_pre.has_error()) {
    return wh::core::result<invoke_config>::failure(interrupt_pre.error());
  }
  if (interrupt_pre.value() != nullptr) {
    config.interrupt_pre_hook = *interrupt_pre.value();
  }

  auto interrupt_post = read_optional_context_value<graph_interrupt_node_hook>(
      context, graph_interrupt_post_hook_session_key);
  if (interrupt_post.has_error()) {
    return wh::core::result<invoke_config>::failure(interrupt_post.error());
  }
  if (interrupt_post.value() != nullptr) {
    config.interrupt_post_hook = *interrupt_post.value();
  }

  auto resume_contexts =
      read_optional_context_value<std::vector<wh::core::interrupt_context>>(
          context, graph_resume_contexts_session_key);
  if (resume_contexts.has_error()) {
    return wh::core::result<invoke_config>::failure(resume_contexts.error());
  }
  if (resume_contexts.value() != nullptr) {
    config.resume_contexts = *resume_contexts.value();
  }

  auto subgraph_signals =
      read_optional_context_value<std::vector<wh::core::interrupt_signal>>(
          context, graph_subgraph_interrupt_signals_session_key);
  if (subgraph_signals.has_error()) {
    return wh::core::result<invoke_config>::failure(subgraph_signals.error());
  }
  if (subgraph_signals.value() != nullptr) {
    config.subgraph_interrupt_signals = *subgraph_signals.value();
  }

  auto resume_decision =
      read_optional_context_value<interrupt_resume_decision>(
          context, graph_resume_decision_session_key);
  if (resume_decision.has_error()) {
    return wh::core::result<invoke_config>::failure(resume_decision.error());
  }
  if (resume_decision.value() != nullptr) {
    config.resume_decision = *resume_decision.value();
  }

  auto batch_items =
      read_optional_context_value<std::vector<resume_batch_item>>(
          context, graph_resume_batch_session_key);
  if (batch_items.has_error()) {
    return wh::core::result<invoke_config>::failure(batch_items.error());
  }
  if (batch_items.value() != nullptr) {
    config.batch_resume_items = *batch_items.value();
  }

  auto reinterrupt = read_optional_context_value<bool>(
      context, graph_resume_reinterrupt_session_key);
  if (reinterrupt.has_error()) {
    return wh::core::result<invoke_config>::failure(reinterrupt.error());
  }
  if (reinterrupt.value() != nullptr) {
    config.reinterrupt_unmatched = *reinterrupt.value();
  }

  auto cache_scope =
      read_optional_context_value<std::string>(context,
                                               schedule_runtime::graph_cache_scope_session_key);
  if (cache_scope.has_error()) {
    return wh::core::result<invoke_config>::failure(cache_scope.error());
  }
  if (cache_scope.value() != nullptr) {
    config.cache_scope_override = *cache_scope.value();
  }

  auto pregel_steps =
      read_optional_context_value<std::size_t>(
          context, schedule_runtime::graph_pregel_max_steps_session_key);
  if (pregel_steps.has_error()) {
    return wh::core::result<invoke_config>::failure(pregel_steps.error());
  }
  if (pregel_steps.value() != nullptr) {
    config.pregel_max_steps_override = *pregel_steps.value();
  }

  auto strategy =
      read_optional_context_value<graph_branch_merge>(
          context, graph_branch_merge_session_key);
  if (strategy.has_error()) {
    return wh::core::result<invoke_config>::failure(strategy.error());
  }
  if (strategy.value() != nullptr) {
    config.branch_merge = *strategy.value();
  }

  return config;
}

inline auto publish_outputs(wh::core::run_context &context,
                            invoke_outputs &&outputs) -> void {
  if (outputs.publish_transition_log) {
    wh::core::set_session_value(
        context, std::string{graph_transition_log_session_key},
        std::move(outputs.transition_log));
  }
  wh::core::set_session_value(
      context, std::string{graph_runtime_last_completed_nodes_session_key},
      std::move(outputs.last_completed_nodes));
  if (!outputs.debug_events.empty()) {
    wh::core::set_session_value(context, std::string{graph_debug_stream_session_key},
                                std::move(outputs.debug_events));
  }
  if (!outputs.state_snapshot_events.empty()) {
    wh::core::set_session_value(
        context, std::string{graph_state_snapshot_stream_session_key},
        std::move(outputs.state_snapshot_events));
  }
  if (!outputs.state_delta_events.empty()) {
    wh::core::set_session_value(
        context, std::string{graph_state_delta_stream_session_key},
        std::move(outputs.state_delta_events));
  }
  if (!outputs.message_events.empty()) {
    wh::core::set_session_value(context, std::string{graph_message_stream_session_key},
                                std::move(outputs.message_events));
  }
  if (!outputs.custom_events.empty()) {
    wh::core::set_session_value(context, std::string{graph_custom_stream_session_key},
                                std::move(outputs.custom_events));
  }
  if (outputs.step_limit_error.has_value()) {
    wh::core::set_session_value(context, std::string{graph_step_limit_error_session_key},
                                std::move(*outputs.step_limit_error));
  }
  if (outputs.node_timeout_error.has_value()) {
    wh::core::set_session_value(
        context, std::string{graph_node_timeout_error_session_key},
        std::move(*outputs.node_timeout_error));
  }
  if (outputs.node_run_error.has_value()) {
    wh::core::set_session_value(context, std::string{graph_node_run_error_session_key},
                                std::move(*outputs.node_run_error));
  }
  if (outputs.graph_run_error.has_value()) {
    wh::core::set_session_value(context, std::string{graph_run_error_session_key},
                                std::move(*outputs.graph_run_error));
  }
  if (outputs.stream_read_error.has_value()) {
    wh::core::set_session_value(
        context, std::string{graph_new_stream_read_error_session_key},
        std::move(*outputs.stream_read_error));
  }
  if (outputs.external_interrupt_resolution.has_value()) {
    wh::core::set_session_value(
        context, std::string{graph_external_interrupt_resolution_session_key},
        std::move(*outputs.external_interrupt_resolution));
  }
}

inline auto merge_nested_outputs(invoke_outputs &target,
                                 invoke_outputs &&nested) -> void {
  target.publish_transition_log =
      target.publish_transition_log || nested.publish_transition_log;
  target.transition_log.insert(target.transition_log.end(),
                               std::make_move_iterator(nested.transition_log.begin()),
                               std::make_move_iterator(nested.transition_log.end()));
  target.debug_events.insert(target.debug_events.end(),
                             std::make_move_iterator(nested.debug_events.begin()),
                             std::make_move_iterator(nested.debug_events.end()));
  target.state_snapshot_events.insert(
      target.state_snapshot_events.end(),
      std::make_move_iterator(nested.state_snapshot_events.begin()),
      std::make_move_iterator(nested.state_snapshot_events.end()));
  target.state_delta_events.insert(
      target.state_delta_events.end(),
      std::make_move_iterator(nested.state_delta_events.begin()),
      std::make_move_iterator(nested.state_delta_events.end()));
  target.message_events.insert(target.message_events.end(),
                               std::make_move_iterator(nested.message_events.begin()),
                               std::make_move_iterator(nested.message_events.end()));
  target.custom_events.insert(target.custom_events.end(),
                              std::make_move_iterator(nested.custom_events.begin()),
                              std::make_move_iterator(nested.custom_events.end()));

  if (!target.step_limit_error.has_value() && nested.step_limit_error.has_value()) {
    target.step_limit_error = std::move(nested.step_limit_error);
  }
  if (!target.node_timeout_error.has_value() &&
      nested.node_timeout_error.has_value()) {
    target.node_timeout_error = std::move(nested.node_timeout_error);
  }
  if (!target.node_run_error.has_value() && nested.node_run_error.has_value()) {
    target.node_run_error = std::move(nested.node_run_error);
  }
  if (!target.graph_run_error.has_value() && nested.graph_run_error.has_value()) {
    target.graph_run_error = std::move(nested.graph_run_error);
  }
  if (!target.stream_read_error.has_value() &&
      nested.stream_read_error.has_value()) {
    target.stream_read_error = std::move(nested.stream_read_error);
  }
  if (!target.external_interrupt_resolution.has_value() &&
      nested.external_interrupt_resolution.has_value()) {
    target.external_interrupt_resolution =
        std::move(nested.external_interrupt_resolution);
  }
}

} // namespace wh::compose::detail::runtime_state

namespace wh::compose {

inline constexpr std::string_view graph_runtime_last_completed_nodes_session_key =
    detail::runtime_state::graph_runtime_last_completed_nodes_session_key;

} // namespace wh::compose
