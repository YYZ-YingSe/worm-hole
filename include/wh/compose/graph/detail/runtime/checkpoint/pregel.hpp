// Defines Pregel-specific checkpoint runtime helpers.
#pragma once

#include "wh/compose/graph/detail/runtime/checkpoint/core.hpp"

namespace wh::compose::detail::checkpoint_runtime {

[[nodiscard]] inline auto capture_pregel_runtime(
    input_runtime::io_storage &storage,
    input_runtime::pregel_delivery_store &pregel_delivery,
    const bool current_superstep_active)
    -> wh::core::result<checkpoint_pregel_runtime_state> {
  auto captured_io = capture_runtime_io(storage);
  if (captured_io.has_error()) {
    return wh::core::result<checkpoint_pregel_runtime_state>::failure(
        captured_io.error());
  }
  auto io = std::move(captured_io).value();

  checkpoint_pregel_runtime_state pregel{};
  pregel.node_outputs = std::move(io.node_outputs);
  pregel.edge_values = std::move(io.edge_values);
  pregel.edge_readers = std::move(io.edge_readers);
  pregel.merged_readers = std::move(io.merged_readers);
  pregel.merged_reader_lanes = std::move(io.merged_reader_lanes);
  pregel.final_output_reader = std::move(io.final_output_reader);
  pregel.current_frontier = pregel_delivery.current_nodes;
  pregel.next_frontier = pregel_delivery.next_nodes;
  pregel.current_superstep_active = current_superstep_active;

  pregel.current_deliveries.reserve(pregel_delivery.current_nodes.size());
  for (const auto node_id : pregel_delivery.current_nodes) {
    const auto &inputs = pregel_delivery.current[node_id];
    pregel.current_deliveries.push_back(checkpoint_pregel_delivery{
        .node_id = node_id,
        .control_edges = inputs.control_edges,
        .data_edges = inputs.data_edges,
    });
  }

  pregel.next_deliveries.reserve(pregel_delivery.next_nodes.size());
  for (const auto node_id : pregel_delivery.next_nodes) {
    const auto &inputs = pregel_delivery.next[node_id];
    pregel.next_deliveries.push_back(checkpoint_pregel_delivery{
        .node_id = node_id,
        .control_edges = inputs.control_edges,
        .data_edges = inputs.data_edges,
    });
  }

  return pregel;
}

inline auto restore_pregel_runtime_io(
    input_runtime::io_storage &storage,
    const std::span<const graph_node_state> lifecycle,
    checkpoint_pregel_runtime_state &pregel) -> wh::core::result<void> {
  for (const auto &state : lifecycle) {
    if (state.node_id >= storage.node_values.size()) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    if (state.lifecycle == graph_node_lifecycle_state::completed) {
      storage.output_valid.set(state.node_id);
    }
  }

  return restore_runtime_slots(storage, pregel.node_outputs, pregel.edge_values,
                               pregel.edge_readers, pregel.merged_readers,
                               pregel.merged_reader_lanes,
                               pregel.final_output_reader);
}

inline auto restore_pregel_runtime(
    checkpoint_pregel_runtime_state &pregel, input_runtime::io_storage &storage,
    const std::span<const graph_node_state> lifecycle,
    input_runtime::pregel_delivery_store &pregel_delivery,
    const std::size_t node_count, bool &current_superstep_active)
    -> wh::core::result<void> {
  auto restored_io = restore_pregel_runtime_io(storage, lifecycle, pregel);
  if (restored_io.has_error()) {
    return restored_io;
  }

  std::vector<input_runtime::pregel_node_inputs> current_inputs(node_count);
  for (auto &delivery : pregel.current_deliveries) {
    if (delivery.node_id >= current_inputs.size()) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    current_inputs[delivery.node_id] = input_runtime::pregel_node_inputs{
        .control_edges = std::move(delivery.control_edges),
        .data_edges = std::move(delivery.data_edges),
    };
  }

  std::vector<input_runtime::pregel_node_inputs> next_inputs(node_count);
  for (auto &delivery : pregel.next_deliveries) {
    if (delivery.node_id >= next_inputs.size()) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    next_inputs[delivery.node_id] = input_runtime::pregel_node_inputs{
        .control_edges = std::move(delivery.control_edges),
        .data_edges = std::move(delivery.data_edges),
    };
  }

  pregel_delivery.restore(node_count, std::move(current_inputs),
                          std::move(next_inputs),
                          std::move(pregel.current_frontier),
                          std::move(pregel.next_frontier));
  current_superstep_active = pregel.current_superstep_active;
  return {};
}

inline auto mark_restored_pregel_pending_nodes(
    runtime_state::pending_inputs &pending_inputs,
    const checkpoint_pregel_runtime_state &pregel,
    const std::size_t node_count) -> wh::core::result<void> {
  detail::dynamic_bitset seen{};
  seen.reset(node_count, false);
  const auto mark_one = [&pending_inputs, &seen, node_count](
                            const std::uint32_t node_id)
      -> wh::core::result<void> {
    if (node_id >= node_count) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    if (seen.set_if_unset(node_id)) {
      pending_inputs.mark_restored_node(node_id);
    }
    return {};
  };

  for (const auto node_id : pregel.current_frontier) {
    auto marked = mark_one(node_id);
    if (marked.has_error()) {
      return marked;
    }
  }
  for (const auto node_id : pregel.next_frontier) {
    auto marked = mark_one(node_id);
    if (marked.has_error()) {
      return marked;
    }
  }
  return {};
}

} // namespace wh::compose::detail::checkpoint_runtime
