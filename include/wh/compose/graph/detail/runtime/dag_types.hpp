// Defines DAG-specific runtime state and storage types.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "wh/compose/graph/detail/runtime/common_input_types.hpp"

namespace wh::compose::detail::input_runtime {

using dag_edge_status = input_edge_status;

enum class dag_ready_state : std::uint8_t { waiting, ready, skipped };

struct dag_branch_state {
  bool decided{false};
  std::vector<std::uint32_t> selected_end_nodes_sorted{};
};

struct dag_schedule_state {
  std::vector<dag_branch_state> branch_states{};
  std::vector<std::uint32_t> decided_branch_nodes{};

  auto reset(const std::size_t node_count) -> void {
    if (branch_states.size() < node_count) {
      branch_states.resize(node_count);
    }
    for (const auto node_id : decided_branch_nodes) {
      auto &state = branch_states[node_id];
      state.decided = false;
      state.selected_end_nodes_sorted.clear();
    }
    decided_branch_nodes.clear();
  }

  auto mark_branch_decided(const std::uint32_t node_id,
                           std::vector<std::uint32_t> &&selected) -> void {
    auto &state = branch_states[node_id];
    if (!state.decided) {
      decided_branch_nodes.push_back(node_id);
    }
    state.decided = true;
    state.selected_end_nodes_sorted = std::move(selected);
  }
};

} // namespace wh::compose::detail::input_runtime
