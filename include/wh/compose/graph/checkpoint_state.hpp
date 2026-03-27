// Defines versioned checkpoint payload for compose graph state/resume recovery.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "wh/compose/graph/restore_shape.hpp"
#include "wh/compose/runtime/state.hpp"
#include "wh/compose/types.hpp"
#include "wh/core/resume_state.hpp"

namespace wh::compose {

/// Versioned checkpoint snapshot used by compose restore/migration flows.
struct checkpoint_state {
  /// Payload schema version (migration source/target key).
  std::uint32_t version{1U};
  /// Stable checkpoint id used by store read/write/index layers.
  std::string checkpoint_id{};
  /// Branch replay key for time-travel and branch-compare debugging.
  std::string branch{"main"};
  /// Optional parent branch key for branch lineage replay.
  std::optional<std::string> parent_branch{};
  /// Restore-stable graph shape bound to this checkpoint payload.
  graph_restore_shape restore_shape{};
  /// Runtime node states captured from scheduler.
  std::vector<graph_node_state> node_states{};
  /// Resume snapshot captured for interrupted nodes.
  wh::core::resume_state resume_snapshot{};
  /// Interrupt-id to address/state snapshot captured for resume handoff.
  wh::core::interrupt_snapshot interrupt_snapshot{};
  /// Rerun input payloads keyed by node key.
  graph_value_map rerun_inputs{};
};

} // namespace wh::compose
