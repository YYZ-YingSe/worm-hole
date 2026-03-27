// Defines public graph runtime/trigger/fan-in policy enums.
#pragma once

#include <cstdint>

namespace wh::compose {

/// Runtime compile/execute mode for graph topology.
enum class graph_runtime_mode : std::uint8_t {
  /// Strict DAG mode: cycles are rejected at compile time.
  dag = 0U,
  /// Pregel-like mode: cycles are allowed and bounded by step budget.
  pregel,
};

/// Node trigger policy used by compile-time topology semantics.
enum class graph_trigger_mode : std::uint8_t {
  /// Node may run when any active predecessor becomes ready.
  any_predecessor = 0U,
  /// Node runs only after all active predecessors become ready.
  all_predecessors,
};

/// Multi-source fan-in behavior used by compile-time merge semantics.
enum class graph_fan_in_policy : std::uint8_t {
  /// Fan-in allows partial source completion.
  allow_partial = 0U,
  /// Fan-in requires all sources complete.
  require_all_sources,
  /// Fan-in requires all sources and source EOF visibility.
  require_all_sources_with_eof,
};

} // namespace wh::compose
