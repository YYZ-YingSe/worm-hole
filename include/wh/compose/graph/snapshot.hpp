// Defines compile-stable graph metadata snapshots used by diff and restore
// validation.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "wh/compose/graph/compile_info.hpp"
#include "wh/compose/graph/edge_lowering.hpp"
#include "wh/compose/types.hpp"
#include "wh/core/type_traits.hpp"

namespace wh::compose {

/// Compile-visible graph options captured for graph diffing.
struct graph_snapshot_compile_options {
  /// Human-readable graph name.
  std::string name{"graph"};
  /// Compile-visible graph boundary.
  graph_boundary boundary{};
  /// Runtime mode used by this compiled graph snapshot.
  graph_runtime_mode mode{graph_runtime_mode::dag};
  /// DAG frontier dispatch policy.
  graph_dispatch_policy dispatch_policy{graph_dispatch_policy::same_wave};
  /// Compile-time max-step budget.
  std::size_t max_steps{1024U};
  /// True keeps cold authoring data after compile.
  bool retain_cold_data{true};
  /// Graph-level trigger mode.
  graph_trigger_mode trigger_mode{graph_trigger_mode::any_predecessor};
  /// Graph-level fan-in policy.
  graph_fan_in_policy fan_in_policy{graph_fan_in_policy::allow_partial};
  /// Default retry budget inherited by nodes.
  std::size_t retry_budget{0U};
  /// Default node timeout budget.
  std::optional<std::chrono::milliseconds> node_timeout{};
  /// Global parallel-node limit.
  std::size_t max_parallel_nodes{1U};
  /// Default per-node parallel limit.
  std::size_t max_parallel_per_node{1U};
  /// True enables local-state generation capability.
  bool enable_local_state_generation{true};
  /// True when a compile callback is installed.
  bool has_compile_callback{false};
};

/// One compile-visible node snapshot used by diff and restore validation.
struct graph_snapshot_node {
  /// Stable node key.
  std::string key{};
  /// Stable runtime node id.
  std::uint32_t node_id{0U};
  /// Public node family.
  node_kind kind{node_kind::component};
  /// Sync/async execution mode.
  node_exec_mode exec_mode{node_exec_mode::sync};
  /// Whether exec-mode came from authored or lowered semantics.
  node_exec_origin exec_origin{node_exec_origin::authored};
  /// Input contract consumed by this node.
  node_contract input_contract{node_contract::value};
  /// Output contract produced by this node.
  node_contract output_contract{node_contract::value};
  /// Compile-visible node option snapshot.
  graph_compile_node_options_info options{};
};

/// One compile-visible edge snapshot used by diff and restore validation.
struct graph_snapshot_edge {
  /// Source node key.
  std::string from{};
  /// Target node key.
  std::string to{};
  /// True disables control dependency on this edge.
  bool no_control{false};
  /// True disables data dependency on this edge.
  bool no_data{false};
  /// Compile-resolved lowering family.
  edge_lowering_kind lowering_kind{edge_lowering_kind::none};
  /// True when this edge uses one authored custom lowering callback.
  bool has_custom_lowering{false};
  /// Runtime guardrails visible on this edge.
  edge_limits limits{};
};

/// One branch destination-set snapshot used by routing diff checks.
struct graph_snapshot_branch {
  /// Branch source node key.
  std::string from{};
  /// Declared destination node keys.
  std::vector<std::string> end_nodes{};
};

/// One immutable compile-stable graph snapshot used by diff and restore
/// validation.
struct graph_snapshot {
  /// Graph-level compile-visible options.
  graph_snapshot_compile_options compile_options{};
  /// Stable key->node-id mapping that survives `retain_cold_data=false`.
  std::unordered_map<std::string, std::uint32_t,
                     wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      node_key_to_id{};
  /// Stable node-id->key mapping.
  std::vector<std::string> node_id_to_key{};
  /// Compile-visible node snapshots.
  std::vector<graph_snapshot_node> nodes{};
  /// Compile-visible edge snapshots.
  std::vector<graph_snapshot_edge> edges{};
  /// Declared branch destination-set snapshots.
  std::vector<graph_snapshot_branch> branches{};
  /// Nested subgraph snapshots keyed by parent node key.
  std::unordered_map<std::string, graph_snapshot,
                     wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      subgraphs{};
};

} // namespace wh::compose
