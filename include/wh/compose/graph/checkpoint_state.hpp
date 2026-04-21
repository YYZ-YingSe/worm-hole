// Defines checkpoint payload for compose graph state/resume recovery.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "wh/compose/graph/restore_shape.hpp"
#include "wh/compose/runtime/state.hpp"
#include "wh/compose/types.hpp"
#include "wh/core/resume_state.hpp"

namespace wh::compose {

/// Persisted runtime payload bound to one numbered storage slot.
struct checkpoint_runtime_slot {
  /// Runtime-local slot id for this payload.
  std::uint32_t slot_id{0U};
  /// Stored value or reader payload for this slot.
  graph_value value{};
};

/// Persisted node input retained across one runtime restore boundary.
struct checkpoint_node_input {
  /// Stable compile-time node id.
  std::uint32_t node_id{0U};
  /// Stable node key kept for diagnostics and validation.
  std::string key{};
  /// Exact execution input retained for this node.
  graph_value input{};
};

/// Persisted pending execution inputs retained inside one mode-local runtime
/// snapshot.
struct checkpoint_pending_inputs {
  /// Entry input retained across the restore boundary when the start node has
  /// not fully drained it yet.
  std::optional<graph_value> entry{};
  /// Exact node inputs retained for nodes that have not re-entered execution
  /// yet.
  std::vector<checkpoint_node_input> nodes{};
};

/// Persisted merged-reader lane state.
enum class checkpoint_reader_lane_state : std::uint8_t {
  unseen = 0U,
  attached,
  disabled,
};

/// Persisted merged-reader lane record keyed by edge id.
struct checkpoint_reader_lane {
  /// Stable edge id bound to this lane.
  std::uint32_t edge_id{0U};
  /// Lane readiness/attachment state at checkpoint time.
  checkpoint_reader_lane_state state{checkpoint_reader_lane_state::unseen};
};

/// Persisted DAG branch decision for one branch node.
struct checkpoint_dag_branch_state {
  /// Stable compile-time node id of the branch node.
  std::uint32_t node_id{0U};
  /// True when this branch already produced a routing decision.
  bool decided{false};
  /// Selected end-node ids sorted by compile order.
  std::vector<std::uint32_t> selected_end_nodes_sorted{};
};

/// Persisted DAG-specific runtime section.
struct checkpoint_dag_runtime_state {
  /// Pending execution inputs owned by the DAG runtime snapshot.
  checkpoint_pending_inputs pending_inputs{};
  /// Materialized node outputs retained by runtime.
  std::vector<checkpoint_runtime_slot> node_outputs{};
  /// Materialized edge value payloads retained by runtime.
  std::vector<checkpoint_runtime_slot> edge_values{};
  /// Live edge readers retained by runtime.
  std::vector<checkpoint_runtime_slot> edge_readers{};
  /// Merged source readers retained by runtime.
  std::vector<checkpoint_runtime_slot> merged_readers{};
  /// Merged-reader lane states keyed by edge id.
  std::vector<checkpoint_reader_lane> merged_reader_lanes{};
  /// Final output reader payload retained for stream boundary graphs when present.
  std::optional<graph_value> final_output_reader{};
  /// Per-branch routing decisions retained by the DAG scheduler.
  std::vector<checkpoint_dag_branch_state> branch_states{};
  /// Current-wave frontier nodes not yet fully drained.
  std::vector<std::uint32_t> current_frontier{};
  /// Next-wave frontier nodes already queued.
  std::vector<std::uint32_t> next_frontier{};
  /// Read head inside `current_frontier`.
  std::size_t current_frontier_head{0U};
  /// Nodes dequeued from the current wave but suspended before entering
  /// execution.
  std::vector<std::uint32_t> suspended_nodes{};
};

/// Persisted Pregel delivery record for one node.
struct checkpoint_pregel_delivery {
  /// Stable compile-time node id receiving this delivery set.
  std::uint32_t node_id{0U};
  /// Control edges delivered to this node.
  std::vector<std::uint32_t> control_edges{};
  /// Data edges delivered to this node.
  std::vector<std::uint32_t> data_edges{};
};

/// Persisted Pregel-specific runtime section.
struct checkpoint_pregel_runtime_state {
  /// Pending execution inputs owned by the Pregel runtime snapshot.
  checkpoint_pending_inputs pending_inputs{};
  /// Materialized node outputs retained by runtime.
  std::vector<checkpoint_runtime_slot> node_outputs{};
  /// Materialized edge value payloads retained by runtime.
  std::vector<checkpoint_runtime_slot> edge_values{};
  /// Live edge readers retained by runtime.
  std::vector<checkpoint_runtime_slot> edge_readers{};
  /// Merged source readers retained by runtime.
  std::vector<checkpoint_runtime_slot> merged_readers{};
  /// Merged-reader lane states keyed by edge id.
  std::vector<checkpoint_reader_lane> merged_reader_lanes{};
  /// Final output reader payload retained for stream boundary graphs when present.
  std::optional<graph_value> final_output_reader{};
  /// Current-superstep delivery buckets keyed by node id.
  std::vector<checkpoint_pregel_delivery> current_deliveries{};
  /// Next-superstep delivery buckets keyed by node id.
  std::vector<checkpoint_pregel_delivery> next_deliveries{};
  /// Current frontier nodes scheduled in this superstep.
  std::vector<std::uint32_t> current_frontier{};
  /// Next frontier nodes already scheduled for the next superstep.
  std::vector<std::uint32_t> next_frontier{};
  /// True when this checkpoint was captured after current superstep began.
  bool current_superstep_active{false};
};

/// Persisted runtime restore section shared by all checkpoint payloads.
struct checkpoint_runtime_state {
  /// Scheduler step counter captured at checkpoint time.
  std::size_t step_count{0U};
  /// Lifecycle snapshot captured from graph state table at checkpoint time.
  std::vector<graph_node_state> lifecycle{};
  /// Optional DAG-specific runtime snapshot.
  std::optional<checkpoint_dag_runtime_state> dag{};
  /// Optional Pregel-specific runtime snapshot.
  std::optional<checkpoint_pregel_runtime_state> pregel{};
};

/// Checkpoint snapshot used by compose restore flows.
struct checkpoint_state {
  /// Stable checkpoint id used by store read/write/index layers.
  std::string checkpoint_id{};
  /// Branch replay key for time-travel and branch-compare debugging.
  std::string branch{"main"};
  /// Optional parent branch key for branch lineage replay.
  std::optional<std::string> parent_branch{};
  /// Restore-stable graph shape bound to this checkpoint payload.
  graph_restore_shape restore_shape{};
  /// Resume snapshot captured for interrupted nodes.
  wh::core::resume_state resume_snapshot{};
  /// Interrupt-id to address/state snapshot captured for resume handoff.
  wh::core::interrupt_snapshot interrupt_snapshot{};
  /// Persisted runtime restore payload.
  checkpoint_runtime_state runtime{};
};

} // namespace wh::compose
