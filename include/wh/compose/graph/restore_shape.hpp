// Defines restore-stable graph shape snapshots used by checkpoint validation.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "wh/compose/graph/policy.hpp"
#include "wh/compose/graph/snapshot.hpp"
#include "wh/compose/types.hpp"
#include "wh/core/type_traits.hpp"

namespace wh::compose {

/// Graph-level restore-relevant options captured in checkpoint payloads.
struct graph_restore_options {
  /// Graph boundary contract.
  graph_boundary boundary{};
  /// Runtime mode that shapes scheduler semantics.
  graph_runtime_mode mode{graph_runtime_mode::dag};
  /// Trigger mode that shapes readiness semantics.
  graph_trigger_mode trigger_mode{graph_trigger_mode::any_predecessor};
  /// Fan-in mode that shapes readiness semantics.
  graph_fan_in_policy fan_in_policy{graph_fan_in_policy::allow_partial};
};

/// Restore-stable node shape captured in checkpoint payloads.
struct graph_restore_node {
  /// Stable node key.
  std::string key{};
  /// Public node family.
  node_kind kind{node_kind::component};
  /// Input contract consumed by this node.
  node_contract input_contract{node_contract::value};
  /// True means node may execute without one control predecessor.
  bool allow_no_control{false};
  /// True means node may execute without one data predecessor.
  bool allow_no_data{false};
};

/// Restore-stable edge shape captured in checkpoint payloads.
struct graph_restore_edge {
  /// Source node key.
  std::string from{};
  /// Target node key.
  std::string to{};
  /// True disables control dependency on this edge.
  bool no_control{false};
  /// True disables data dependency on this edge.
  bool no_data{false};
  /// Selected adapter family.
  edge_adapter_kind adapter_kind{edge_adapter_kind::none};
  /// True when a custom value->stream adapter exists.
  bool has_custom_value_to_stream{false};
  /// True when a custom stream->value adapter exists.
  bool has_custom_stream_to_value{false};
};

/// Restore-stable branch shape captured in checkpoint payloads.
struct graph_restore_branch {
  /// Branch source node key.
  std::string from{};
  /// Allowed destination node keys.
  std::vector<std::string> end_nodes{};
};

/// Immutable restore-stable graph shape used by checkpoint validation.
struct graph_restore_shape {
  /// Graph-level restore-relevant options.
  graph_restore_options options{};
  /// Restore-stable node set.
  std::vector<graph_restore_node> nodes{};
  /// Restore-stable edge set.
  std::vector<graph_restore_edge> edges{};
  /// Restore-stable branch set.
  std::vector<graph_restore_branch> branches{};
  /// Nested subgraph restore shapes keyed by parent node key.
  std::unordered_map<std::string, graph_restore_shape,
                     wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      subgraphs{};
};

namespace detail {

[[nodiscard]] inline auto restore_node_less(const graph_restore_node &lhs,
                                            const graph_restore_node &rhs)
    noexcept -> bool {
  return lhs.key < rhs.key;
}

[[nodiscard]] inline auto restore_edge_less(const graph_restore_edge &lhs,
                                            const graph_restore_edge &rhs)
    noexcept -> bool {
  return std::tie(lhs.from, lhs.to) < std::tie(rhs.from, rhs.to);
}

[[nodiscard]] inline auto restore_branch_less(const graph_restore_branch &lhs,
                                              const graph_restore_branch &rhs)
    noexcept -> bool {
  return lhs.from < rhs.from;
}

[[nodiscard]] inline auto to_restore_shape(const graph_snapshot &snapshot)
    -> graph_restore_shape {
  graph_restore_shape shape{};
  shape.options = graph_restore_options{
      .boundary = snapshot.compile_options.boundary,
      .mode = snapshot.compile_options.mode,
      .trigger_mode = snapshot.compile_options.trigger_mode,
      .fan_in_policy = snapshot.compile_options.fan_in_policy,
  };

  shape.nodes.reserve(snapshot.nodes.size());
  for (const auto &node : snapshot.nodes) {
    shape.nodes.push_back(graph_restore_node{
        .key = node.key,
        .kind = node.kind,
        .input_contract = node.input_contract,
        .allow_no_control = node.options.allow_no_control,
        .allow_no_data = node.options.allow_no_data,
    });
  }
  std::sort(shape.nodes.begin(), shape.nodes.end(), restore_node_less);

  shape.edges.reserve(snapshot.edges.size());
  for (const auto &edge : snapshot.edges) {
    shape.edges.push_back(graph_restore_edge{
        .from = edge.from,
        .to = edge.to,
        .no_control = edge.no_control,
        .no_data = edge.no_data,
        .adapter_kind = edge.adapter_kind,
        .has_custom_value_to_stream = edge.has_custom_value_to_stream,
        .has_custom_stream_to_value = edge.has_custom_stream_to_value,
    });
  }
  std::sort(shape.edges.begin(), shape.edges.end(), restore_edge_less);

  shape.branches.reserve(snapshot.branches.size());
  for (const auto &branch : snapshot.branches) {
    auto branch_shape = graph_restore_branch{
        .from = branch.from,
        .end_nodes = branch.end_nodes,
    };
    std::sort(branch_shape.end_nodes.begin(), branch_shape.end_nodes.end());
    shape.branches.push_back(std::move(branch_shape));
  }
  std::sort(shape.branches.begin(), shape.branches.end(), restore_branch_less);

  for (const auto &[key, subgraph] : snapshot.subgraphs) {
    shape.subgraphs.emplace(key, to_restore_shape(subgraph));
  }
  return shape;
}

} // namespace detail

} // namespace wh::compose
