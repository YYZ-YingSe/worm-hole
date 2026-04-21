// Defines typed invoke-time config/runtime state used by compose graph
// execution.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wh/compose/graph/call_options.hpp"
#include "wh/compose/graph/error.hpp"
#include "wh/compose/node/execution.hpp"
#include "wh/compose/runtime/checkpoint.hpp"
#include "wh/compose/runtime/interrupt.hpp"
#include "wh/compose/runtime/resume.hpp"
#include "wh/compose/runtime/state.hpp"
#include "wh/core/any.hpp"
#include "wh/core/run_context.hpp"

namespace wh::compose::detail::runtime_state {

/// Snapshot of run-scoped external graph configuration resolved once at invoke
/// start.
struct invoke_config {
  /// Optional state-handler registry injected by caller.
  const graph_state_handler_registry *state_handlers{nullptr};
  /// Optional checkpoint store injected by caller.
  checkpoint_store *checkpoint_store_ptr{nullptr};
  /// Optional checkpoint backend injected by caller.
  checkpoint_backend *checkpoint_backend_ptr{nullptr};
  /// Optional stream conversion registry used by checkpoint save/load.
  const checkpoint_stream_codecs *checkpoint_stream_codecs_ptr{nullptr};
  /// Optional serializer pair used by checkpoint save/load.
  const checkpoint_serializer *checkpoint_serializer_ptr{nullptr};
  /// Optional restore/load options for this invoke.
  std::optional<checkpoint_load_options> checkpoint_load{};
  /// Optional persist/save options for this invoke.
  std::optional<checkpoint_save_options> checkpoint_save{};
  /// Optional whole-checkpoint modifier applied before restore.
  checkpoint_state_modifier checkpoint_before_load{nullptr};
  /// Optional NodePath-scoped node-state modifiers applied before restore.
  checkpoint_node_hooks checkpoint_before_load_nodes{};
  /// Optional whole-checkpoint modifier applied after restore.
  checkpoint_state_modifier checkpoint_after_load{nullptr};
  /// Optional NodePath-scoped node-state modifiers applied after restore.
  checkpoint_node_hooks checkpoint_after_load_nodes{};
  /// Optional whole-checkpoint modifier applied before persist.
  checkpoint_state_modifier checkpoint_before_save{nullptr};
  /// Optional NodePath-scoped node-state modifiers applied before persist.
  checkpoint_node_hooks checkpoint_before_save_nodes{};
  /// Optional whole-checkpoint modifier applied after persist.
  checkpoint_state_modifier checkpoint_after_save{nullptr};
  /// Optional NodePath-scoped node-state modifiers applied after persist.
  checkpoint_node_hooks checkpoint_after_save_nodes{};
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
  /// Pregel max-step override inherited from ambient context.
  std::optional<std::size_t> pregel_max_steps_override{};
  /// Parallel-branch merge policy for this invoke run.
  graph_branch_merge branch_merge{graph_branch_merge::set_union};
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
  std::vector<graph_state_snapshot_event> state_snapshot_events{};
  /// State-delta stream payloads accumulated during execution.
  std::vector<graph_state_delta_event> state_delta_events{};
  /// Runtime-message stream events accumulated during execution.
  std::vector<graph_runtime_message_event> runtime_message_events{};
  /// Custom stream payloads accumulated during execution.
  std::vector<graph_custom_event> custom_events{};
  /// Remaining forwarded checkpoint keys after nested restore consumption.
  std::vector<std::string> remaining_forwarded_checkpoint_keys{};
  /// Last completed node set published on terminal paths.
  std::vector<std::string> completed_node_keys{};
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
  std::optional<graph_external_interrupt_resolution_kind> external_interrupt_resolution{};
  /// Optional checkpoint error detail captured for this run.
  std::optional<checkpoint_error_detail> checkpoint_error{};
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

/// Invoke-owned node cache resolved once from graph topology and call scope.
struct node_cache_state {
  /// Trace state shared by the graph root and all node attempts.
  graph_trace_state trace{};
  /// Lazily materialized runtime node paths indexed by node id.
  std::vector<node_path> runtime_node_paths{};
  /// Lazily materialized stream scopes indexed by node id.
  std::vector<graph_event_scope> runtime_stream_scopes{};
  /// Lazily materialized runtime node execution addresses indexed by node id.
  std::vector<wh::core::address> runtime_node_execution_addresses{};
  /// Resolved component option maps indexed by node id.
  std::vector<graph_component_option_map> resolved_component_options{};
  /// Resolved observation overrides indexed by node id.
  std::vector<graph_resolved_node_observation> resolved_node_observations{};
  /// Resolved state-handler bindings indexed by node id.
  std::vector<const graph_node_state_handlers *> resolved_state_handlers{};
  /// True when invoke-level component option overrides are present.
  bool has_component_option_overrides{false};
  /// True when debug events should be emitted.
  bool emit_debug_events{false};
  /// True when transition log collection is enabled.
  bool collect_transition_log{false};
  /// True when state snapshot events should be emitted.
  bool emit_state_snapshot_events{false};
  /// True when state delta events should be emitted.
  bool emit_state_delta_events{false};
  /// True when runtime message events should be emitted.
  bool emit_runtime_message_events{false};
  /// True when custom events should be emitted.
  bool emit_custom_events{false};
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

inline auto merge_nested_outputs(invoke_outputs &target, invoke_outputs &&nested) -> void {
  target.publish_transition_log = target.publish_transition_log || nested.publish_transition_log;
  if (target.completed_node_keys.empty() && !nested.completed_node_keys.empty()) {
    target.completed_node_keys = std::move(nested.completed_node_keys);
  }
  target.transition_log.insert(target.transition_log.end(),
                               std::make_move_iterator(nested.transition_log.begin()),
                               std::make_move_iterator(nested.transition_log.end()));
  target.debug_events.insert(target.debug_events.end(),
                             std::make_move_iterator(nested.debug_events.begin()),
                             std::make_move_iterator(nested.debug_events.end()));
  target.state_snapshot_events.insert(target.state_snapshot_events.end(),
                                      std::make_move_iterator(nested.state_snapshot_events.begin()),
                                      std::make_move_iterator(nested.state_snapshot_events.end()));
  target.state_delta_events.insert(target.state_delta_events.end(),
                                   std::make_move_iterator(nested.state_delta_events.begin()),
                                   std::make_move_iterator(nested.state_delta_events.end()));
  target.runtime_message_events.insert(
      target.runtime_message_events.end(),
      std::make_move_iterator(nested.runtime_message_events.begin()),
      std::make_move_iterator(nested.runtime_message_events.end()));
  target.custom_events.insert(target.custom_events.end(),
                              std::make_move_iterator(nested.custom_events.begin()),
                              std::make_move_iterator(nested.custom_events.end()));

  if (!target.step_limit_error.has_value() && nested.step_limit_error.has_value()) {
    target.step_limit_error = std::move(nested.step_limit_error);
  }
  if (!target.node_timeout_error.has_value() && nested.node_timeout_error.has_value()) {
    target.node_timeout_error = std::move(nested.node_timeout_error);
  }
  if (!target.node_run_error.has_value() && nested.node_run_error.has_value()) {
    target.node_run_error = std::move(nested.node_run_error);
  }
  if (!target.graph_run_error.has_value() && nested.graph_run_error.has_value()) {
    target.graph_run_error = std::move(nested.graph_run_error);
  }
  if (!target.stream_read_error.has_value() && nested.stream_read_error.has_value()) {
    target.stream_read_error = std::move(nested.stream_read_error);
  }
  if (!target.external_interrupt_resolution.has_value() &&
      nested.external_interrupt_resolution.has_value()) {
    target.external_interrupt_resolution = std::move(nested.external_interrupt_resolution);
  }
  if (!target.checkpoint_error.has_value() && nested.checkpoint_error.has_value()) {
    target.checkpoint_error = std::move(nested.checkpoint_error);
  }
}

} // namespace wh::compose::detail::runtime_state
