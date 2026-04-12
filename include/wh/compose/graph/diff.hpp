// Defines public graph diff entrypoints.
#pragma once

#include "wh/compose/graph.hpp"
#include "wh/compose/graph/diff_check.hpp"

namespace wh::compose {

/// Returns the compile-visible diff between two compiled graphs.
[[nodiscard]] inline auto diff_graph(const graph &baseline,
                                     const graph &candidate)
    -> wh::core::result<graph_diff> {
  if (!baseline.compiled() || !candidate.compiled()) {
    return wh::core::result<graph_diff>::failure(
        wh::core::errc::contract_violation);
  }
  return diff_graph(baseline.snapshot_view(), candidate.snapshot_view());
}

} // namespace wh::compose
