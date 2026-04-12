// Defines graph edge activity classification used by compose runtime.
#pragma once

#include <concepts>
#include <cstdint>
#include <span>
#include <vector>

#include "wh/compose/types.hpp"

namespace wh::compose {

/// Runtime activity classification for one edge.
enum class edge_activity : std::uint8_t {
  /// Edge source has not produced routing decision yet.
  waiting = 0U,
  /// Edge is active and contributes control/data flow.
  active,
  /// Edge is disabled by branch/skip policy.
  disabled,
};

/// Classified edge sets used by fanout/fanin runtime handling.
struct edge_activity_sets {
  /// Edges currently active.
  std::vector<graph_edge> active{};
  /// Edges currently waiting.
  std::vector<graph_edge> waiting{};
  /// Edges currently disabled.
  std::vector<graph_edge> disabled{};
};

template <typename selector_t>
  requires requires(selector_t selector, const graph_edge &edge) {
    { selector(edge) } -> std::same_as<edge_activity>;
  }
/// Classifies one edge list by external runtime selector policy.
[[nodiscard]] inline auto
classify_edges(const std::span<const graph_edge> edges, selector_t &&selector)
    -> edge_activity_sets {
  edge_activity_sets sets{};
  sets.active.reserve(edges.size());
  sets.waiting.reserve(edges.size());
  sets.disabled.reserve(edges.size());
  for (const auto &edge : edges) {
    const auto status = selector(edge);
    if (status == edge_activity::active) {
      sets.active.push_back(edge);
    } else if (status == edge_activity::waiting) {
      sets.waiting.push_back(edge);
    } else {
      sets.disabled.push_back(edge);
    }
  }
  return sets;
}

} // namespace wh::compose
