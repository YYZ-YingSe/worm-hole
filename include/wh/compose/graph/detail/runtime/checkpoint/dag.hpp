// Defines DAG-specific checkpoint runtime helpers.
#pragma once

#include "wh/compose/graph/detail/dag_frontier.hpp"
#include "wh/compose/graph/detail/runtime/checkpoint/core.hpp"

namespace wh::compose::detail::checkpoint_runtime {

[[nodiscard]] inline auto
to_dag_node_phase(const graph_node_lifecycle_state lifecycle) noexcept
    -> input_runtime::dag_node_phase {
  switch (lifecycle) {
  case graph_node_lifecycle_state::completed:
    return input_runtime::dag_node_phase::executed;
  case graph_node_lifecycle_state::skipped:
    return input_runtime::dag_node_phase::skipped;
  case graph_node_lifecycle_state::pending:
  case graph_node_lifecycle_state::running:
  case graph_node_lifecycle_state::failed:
  case graph_node_lifecycle_state::canceled:
    return input_runtime::dag_node_phase::pending;
  }
  return input_runtime::dag_node_phase::pending;
}

inline auto restore_dag_node_phases(
    const std::span<const graph_node_state> lifecycle,
    std::vector<input_runtime::dag_node_phase> &dag_node_phases)
    -> wh::core::result<void> {
  for (const auto &state : lifecycle) {
    if (state.node_id >= dag_node_phases.size()) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    dag_node_phases[state.node_id] = to_dag_node_phase(state.lifecycle);
  }
  return {};
}

[[nodiscard]] inline auto capture_dag_runtime(
    input_runtime::io_storage &storage, input_runtime::dag_schedule &dag_schedule,
    detail::dag_frontier &frontier,
    std::vector<std::uint32_t> suspended_nodes)
    -> wh::core::result<checkpoint_dag_runtime_state> {
  auto captured_io = capture_runtime_io(storage);
  if (captured_io.has_error()) {
    return wh::core::result<checkpoint_dag_runtime_state>::failure(
        captured_io.error());
  }
  auto io = std::move(captured_io).value();

  checkpoint_dag_runtime_state dag{};
  dag.node_outputs = std::move(io.node_outputs);
  dag.edge_values = std::move(io.edge_values);
  dag.edge_readers = std::move(io.edge_readers);
  dag.merged_readers = std::move(io.merged_readers);
  dag.merged_reader_lanes = std::move(io.merged_reader_lanes);
  dag.final_output_reader = std::move(io.final_output_reader);
  dag.current_frontier = frontier.current_nodes();
  dag.next_frontier = frontier.next_nodes();
  dag.current_frontier_head = frontier.current_head();
  dag.suspended_nodes = std::move(suspended_nodes);
  dag.branch_states.reserve(dag_schedule.decided_branch_nodes.size());
  for (const auto node_id : dag_schedule.decided_branch_nodes) {
    const auto &state = dag_schedule.branch_states[node_id];
    dag.branch_states.push_back(checkpoint_dag_branch_state{
        .node_id = node_id,
        .decided = state.decided,
        .selected_end_nodes_sorted = state.selected_end_nodes_sorted,
    });
  }
  return dag;
}

inline auto restore_dag_runtime_io(
    input_runtime::io_storage &storage,
    const std::span<const graph_node_state> lifecycle,
    checkpoint_dag_runtime_state &dag) -> wh::core::result<void> {
  for (const auto &state : lifecycle) {
    if (state.node_id >= storage.node_values.size()) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    if (state.lifecycle == graph_node_lifecycle_state::completed) {
      storage.output_valid.set(state.node_id);
    }
  }

  return restore_runtime_slots(storage, dag.node_outputs, dag.edge_values,
                               dag.edge_readers, dag.merged_readers,
                               dag.merged_reader_lanes, dag.final_output_reader);
}

inline auto restore_dag_runtime(
    checkpoint_dag_runtime_state &dag, input_runtime::io_storage &storage,
    const std::span<const graph_node_state> lifecycle,
    std::vector<input_runtime::dag_node_phase> &dag_node_phases,
    input_runtime::dag_schedule &dag_schedule, detail::dag_frontier &frontier,
    std::vector<std::uint32_t> &suspended_nodes)
    -> wh::core::result<void> {
  auto restored_io = restore_dag_runtime_io(storage, lifecycle, dag);
  if (restored_io.has_error()) {
    return restored_io;
  }

  dag_schedule.reset(dag_node_phases.size());
  for (auto &branch : dag.branch_states) {
    if (!branch.decided) {
      continue;
    }
    if (branch.node_id >= dag_schedule.branch_states.size()) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    dag_schedule.mark_branch_decided(branch.node_id,
                                     std::move(branch.selected_end_nodes_sorted));
  }

  std::vector<std::uint32_t> restored_current{};
  const auto frontier_head =
      std::min(dag.current_frontier_head, dag.current_frontier.size());
  restored_current.reserve(dag.suspended_nodes.size() +
                           (dag.current_frontier.size() - frontier_head));
  for (const auto node_id : dag.suspended_nodes) {
    restored_current.push_back(node_id);
  }
  for (std::size_t index = frontier_head; index < dag.current_frontier.size();
       ++index) {
    restored_current.push_back(dag.current_frontier[index]);
  }
  frontier.restore(dag_node_phases.size(), std::move(restored_current),
                   std::move(dag.next_frontier), 0U);
  suspended_nodes = std::move(dag.suspended_nodes);
  return {};
}

inline auto mark_restored_dag_pending_nodes(
    runtime_state::pending_inputs &pending_inputs,
    const std::vector<input_runtime::dag_node_phase> &dag_node_phases)
    -> void {
  for (std::uint32_t node_id = 0U;
       node_id < static_cast<std::uint32_t>(dag_node_phases.size());
       ++node_id) {
    if (dag_node_phases[node_id] == input_runtime::dag_node_phase::pending) {
      pending_inputs.mark_restored_node(node_id);
    }
  }
}

} // namespace wh::compose::detail::checkpoint_runtime
