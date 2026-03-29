// Defines the public typed compose invoke request/report surface.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "wh/compose/graph/call_options.hpp"
#include "wh/compose/graph/error.hpp"
#include "wh/compose/reduce/stream_concat.hpp"
#include "wh/compose/reduce/values_merge.hpp"
#include "wh/compose/runtime/checkpoint.hpp"
#include "wh/compose/runtime/interrupt.hpp"
#include "wh/compose/runtime/resume.hpp"
#include "wh/compose/runtime/state.hpp"
#include "wh/compose/types.hpp"
#include "wh/core/result.hpp"

namespace wh::compose {

/// Host-injected runtime services visible to one graph invoke.
struct graph_runtime_services {
  /// Checkpoint-related host capabilities used by restore/persist paths.
  struct checkpoint_services {
    /// In-memory checkpoint store used by restore/save when present.
    checkpoint_store *store{nullptr};
    /// Pluggable checkpoint backend used by restore/save when present.
    checkpoint_backend *backend{nullptr};
    /// Stream conversion registry used by checkpoint save/load.
    const checkpoint_stream_codecs *stream_codecs{nullptr};
    /// Serializer pair used by checkpoint encode/decode.
    const checkpoint_serializer *serializer{nullptr};
  } checkpoint{};

  /// Node state-handler registry used by runtime pre/post hooks.
  const graph_state_handler_registry *state_handlers{nullptr};
  /// Value-merge registry used by fan-in value composition.
  const wh::internal::values_merge_registry *values_merge_registry{nullptr};
  /// Stream-concat registry reserved for fan-in stream composition.
  const wh::internal::stream_concat_registry *stream_concat_registry{nullptr};
};

/// Typed per-invoke controls that should not be encoded through session keys.
struct graph_invoke_controls {
  /// Existing graph call options kept as the top-level user call contract.
  graph_call_options call{};

  /// Checkpoint load/save controls for this invoke.
  struct checkpoint_controls {
    /// Optional restore/load options for this invoke.
    std::optional<checkpoint_load_options> load{};
    /// Optional persist/save options for this invoke.
    std::optional<checkpoint_save_options> save{};
    /// Optional whole-checkpoint modifier applied before restore.
    checkpoint_state_modifier before_load{nullptr};
    /// Optional NodePath-scoped node-state modifiers applied before restore.
    checkpoint_node_hooks before_load_nodes{};
    /// Optional whole-checkpoint modifier applied after restore.
    checkpoint_state_modifier after_load{nullptr};
    /// Optional NodePath-scoped node-state modifiers applied after restore.
    checkpoint_node_hooks after_load_nodes{};
    /// Optional whole-checkpoint modifier applied before persist.
    checkpoint_state_modifier before_save{nullptr};
    /// Optional NodePath-scoped node-state modifiers applied before persist.
    checkpoint_node_hooks before_save_nodes{};
    /// Optional whole-checkpoint modifier applied after persist.
    checkpoint_state_modifier after_save{nullptr};
    /// Optional NodePath-scoped node-state modifiers applied after persist.
    checkpoint_node_hooks after_save_nodes{};
    /// Optional one-shot forwarded checkpoints consumed by nested restore.
    forwarded_checkpoint_map forwarded_once{};
  } checkpoint{};

  /// Resume patch controls for this invoke.
  struct resume_controls {
    /// Optional single audited resume decision.
    std::optional<interrupt_resume_decision> decision{};
    /// Optional batch resume payloads keyed by interrupt-context id.
    std::vector<resume_batch_item> batch_items{};
    /// Optional interrupt contexts participating in this resume flow.
    std::vector<wh::core::interrupt_context> contexts{};
    /// True re-interrupts unmatched contexts after explicit resume patching.
    bool reinterrupt_unmatched{true};
  } resume{};

  /// Scheduler and branch policy controls for this invoke.
  struct schedule_controls {
    /// Optional invoke-local Pregel max-step override.
    std::optional<std::size_t> pregel_max_steps{};
    /// Optional invoke-local branch merge strategy override.
    std::optional<graph_branch_merge> branch_merge{};
  } schedule{};

  /// Interrupt hook controls for this invoke.
  struct interrupt_controls {
    /// Optional pre-node interrupt hook.
    graph_interrupt_node_hook pre_hook{nullptr};
    /// Optional post-node interrupt hook.
    graph_interrupt_node_hook post_hook{nullptr};
    /// Optional subgraph interrupt signals forwarded into this invoke.
    std::vector<wh::core::interrupt_signal> subgraph_signals{};
  } interrupt{};
};

/// Structured runtime report returned after one graph invoke completes.
struct graph_run_report {
  /// Transition log emitted during this invoke.
  graph_transition_log transition_log{};
  /// Terminal-path completed node set.
  std::vector<std::string> last_completed_nodes{};
  /// Debug scheduling events emitted during this invoke.
  std::vector<graph_debug_stream_event> debug_events{};
  /// State snapshot events emitted during this invoke.
  std::vector<graph_state_snapshot_event> state_snapshot_events{};
  /// State delta events emitted during this invoke.
  std::vector<graph_state_delta_event> state_delta_events{};
  /// Runtime message events emitted during this invoke.
  std::vector<graph_runtime_message_event> runtime_message_events{};
  /// Custom channel events emitted during this invoke.
  std::vector<graph_custom_event> custom_events{};
  /// Remaining forwarded checkpoint keys after nested restore consumption.
  std::vector<std::string> remaining_forwarded_checkpoint_keys{};
  /// Optional step-limit error detail captured during this invoke.
  std::optional<graph_step_limit_error_detail> step_limit_error{};
  /// Optional node-timeout detail captured during this invoke.
  std::optional<graph_node_timeout_error_detail> node_timeout_error{};
  /// Optional node-run detail captured during this invoke.
  std::optional<graph_node_run_error_detail> node_run_error{};
  /// Optional graph-run detail captured during this invoke.
  std::optional<graph_run_error_detail> graph_run_error{};
  /// Optional stream-read error detail captured during this invoke.
  std::optional<graph_new_stream_read_error_detail> stream_read_error{};
  /// Optional external interrupt resolution captured during this invoke.
  std::optional<graph_external_interrupt_resolution_kind> interrupt_resolution{};
  /// Optional checkpoint error detail captured during this invoke.
  std::optional<checkpoint_error_detail> checkpoint_error{};
};

/// One public typed graph invoke request.
struct graph_invoke_request {
  /// Graph input payload for this invoke.
  graph_value input{};
  /// Explicit invoke controls for this invoke.
  graph_invoke_controls controls{};
  /// Optional host runtime services visible to this invoke.
  const graph_runtime_services *services{nullptr};
};

/// One public typed graph invoke result that preserves both status and report.
struct graph_invoke_result {
  /// Output status produced by this invoke.
  wh::core::result<graph_value> output_status{};
  /// Structured runtime report collected for this invoke.
  graph_run_report report{};
};

} // namespace wh::compose
