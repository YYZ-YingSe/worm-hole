// Defines Pregel-specific input assembly and step-local delivery helpers.
#pragma once

#include "wh/compose/graph/graph.hpp"

namespace wh::compose {

inline auto graph::classify_pregel_node_readiness(
    const std::uint32_t node_id,
    const pregel_node_inputs &inputs) const -> pregel_ready_state {
  const auto *node = core().compiled_execution_index_.index.nodes_by_id[node_id];
  if (node == nullptr) {
    return pregel_ready_state::skipped;
  }

  const auto total_control_edges =
      core().compiled_execution_index_.index.incoming_control(node_id).size();
  const auto active_control_edges = inputs.control_edges.size();
  auto ready_by_control = false;

  if (core().options_.trigger_mode == graph_trigger_mode::all_predecessors) {
    if (total_control_edges > 0U) {
      ready_by_control = active_control_edges == total_control_edges;
    }
  } else if (active_control_edges > 0U) {
    ready_by_control = true;
  } else if (total_control_edges == 0U && node->meta.options.allow_no_control) {
    ready_by_control = true;
  }

  if (!ready_by_control && total_control_edges == 0U &&
      node->meta.options.allow_no_control) {
    ready_by_control = true;
  }

  return ready_by_control ? pregel_ready_state::ready
                          : pregel_ready_state::skipped;
}

inline auto graph::reset_pregel_source_caches(
    const std::uint32_t source_node_id,
    io_storage &io_storage) const -> void {
  for (const auto edge_id :
       core().compiled_execution_index_.index.outgoing_data(source_node_id)) {
    io_storage.edge_value_valid.clear(edge_id);
    io_storage.edge_reader_valid.clear(edge_id);
  }
}

inline auto graph::seed_pregel_successors(
    const std::uint32_t source_node_id,
    const std::optional<std::vector<std::uint32_t>> &selection,
    pregel_delivery_store &deliveries) const -> void {
  const auto selected = [&selection](const std::uint32_t target_id) noexcept {
    if (!selection.has_value()) {
      return true;
    }
    return std::binary_search(selection->begin(), selection->end(), target_id);
  };

  for (const auto edge_id :
       core().compiled_execution_index_.index.outgoing_control(source_node_id)) {
    const auto &edge = core().compiled_execution_index_.index.indexed_edges[edge_id];
    if (!selected(edge.to) || edge.no_control) {
      continue;
    }
    deliveries.stage_current_control(edge.to, edge_id);
  }

  for (const auto edge_id :
       core().compiled_execution_index_.index.outgoing_data(source_node_id)) {
    const auto &edge = core().compiled_execution_index_.index.indexed_edges[edge_id];
    if (!selected(edge.to)) {
      continue;
    }
    if (!edge.no_data) {
      deliveries.stage_current_data(edge.to, edge_id);
    }
  }
}

inline auto graph::stage_pregel_successors(
    const std::uint32_t source_node_id,
    const std::optional<std::vector<std::uint32_t>> &selection,
    pregel_delivery_store &deliveries) const -> void {
  const auto selected = [&selection](const std::uint32_t target_id) noexcept {
    if (!selection.has_value()) {
      return true;
    }
    return std::binary_search(selection->begin(), selection->end(), target_id);
  };

  for (const auto edge_id :
       core().compiled_execution_index_.index.outgoing_control(source_node_id)) {
    const auto &edge = core().compiled_execution_index_.index.indexed_edges[edge_id];
    if (!selected(edge.to) || edge.no_control) {
      continue;
    }
    deliveries.stage_next_control(edge.to, edge_id);
  }

  for (const auto edge_id :
       core().compiled_execution_index_.index.outgoing_data(source_node_id)) {
    const auto &edge = core().compiled_execution_index_.index.indexed_edges[edge_id];
    if (!selected(edge.to)) {
      continue;
    }
    if (!edge.no_data) {
      deliveries.stage_next_data(edge.to, edge_id);
    }
  }
}

inline auto graph::build_pregel_node_input_sender(
    const std::uint32_t node_id, const pregel_node_inputs &inputs,
    io_storage &io_storage, wh::core::run_context &context,
    attempt_slot *slot,
    [[maybe_unused]] const detail::runtime_state::invoke_config &config,
    const wh::core::detail::any_resume_scheduler_t &graph_scheduler) const
    -> graph_sender {
  if (node_id >= core().compiled_execution_index_.index.nodes_by_id.size()) {
    return detail::failure_graph_sender(wh::core::errc::not_found);
  }
  const auto *node = core().compiled_execution_index_.index.nodes_by_id[node_id];
  if (node == nullptr) {
    return detail::failure_graph_sender(wh::core::errc::not_found);
  }

  const auto finalize_input =
      [this, node, &context](wh::core::result<resolved_input> resolved)
      -> wh::core::result<graph_value> {
    if (resolved.has_error() && resolved.error() == wh::core::errc::not_found &&
        context.resume_info.has_value()) {
      auto fallback = resolve_missing_pending_input(node->meta.input_contract);
      if (fallback.has_error()) {
        return wh::core::result<graph_value>::failure(fallback.error());
      }
      auto lifted = own_input(std::move(fallback).value(),
                              node->meta.input_contract);
      if (lifted.has_error()) {
        return wh::core::result<graph_value>::failure(lifted.error());
      }
      resolved = std::move(lifted).value();
    }
    if (resolved.has_error()) {
      return wh::core::result<graph_value>::failure(resolved.error());
    }
    auto materialized = std::move(resolved).value().materialize();
    if (materialized.has_error()) {
      return wh::core::result<graph_value>::failure(materialized.error());
    }
    return std::move(materialized).value();
  };

  input_lane_vector lanes{};
  lanes.reserve(inputs.data_edges.size());
  for (const auto edge_id : inputs.data_edges) {
    const auto &edge = core().compiled_execution_index_.index.indexed_edges[edge_id];
    lanes.push_back(input_lane{
        .edge_id = edge_id,
        .source_id = edge.from,
        .status = edge_status::active,
        .output_ready = true,
    });
  }

  if (node->meta.input_contract == node_contract::stream) {
    wh::core::result<resolved_input> resolved =
        wh::core::result<resolved_input>::failure(wh::core::errc::not_found);
    if (lanes.empty()) {
      resolved = build_missing_input(*node);
    } else if (lanes.size() == 1U) {
      auto reader = take_edge_reader(lanes.front().edge_id, io_storage);
      if (reader.has_error()) {
        resolved = wh::core::result<resolved_input>::failure(reader.error());
      } else {
        resolved = resolved_input::own_reader(std::move(reader).value());
      }
    } else {
      std::vector<wh::schema::stream::named_stream_reader<graph_stream_reader>>
          readers{};
      readers.reserve(lanes.size());
      for (const auto &lane : lanes) {
        auto reader = take_edge_reader(lane.edge_id, io_storage);
        if (reader.has_error()) {
          resolved = wh::core::result<resolved_input>::failure(reader.error());
          break;
        }
        readers.push_back(wh::schema::stream::named_stream_reader<
                          graph_stream_reader>{
            .source = core().compiled_execution_index_.index.id_to_key[lane.source_id],
            .reader = std::move(reader).value(),
        });
      }
      if (readers.size() == lanes.size()) {
        resolved = resolved_input::own_reader(
            detail::make_graph_merge_reader(std::move(readers)));
      }
    }

    auto finalized = finalize_input(std::move(resolved));
    if (finalized.has_error()) {
      return detail::failure_graph_sender(finalized.error());
    }
    return detail::ready_graph_sender(std::move(finalized));
  }

  const bool preserve_stream_pre =
      slot != nullptr &&
      detail::state_runtime::has_stream_phase(
          slot->state_handlers, detail::state_runtime::state_phase::pre);
  if (preserve_stream_pre && lanes.size() == 1U) {
    const auto &edge =
        core().compiled_execution_index_.index.indexed_edges[lanes.front().edge_id];
    if (needs_reader_lowering(edge)) {
      auto reader = take_edge_reader(lanes.front().edge_id, io_storage);
      if (reader.has_error()) {
        return detail::failure_graph_sender(reader.error());
      }
      auto lowering = make_reader_lowering(edge);
      if (lowering.has_error()) {
        return detail::failure_graph_sender(lowering.error());
      }
      if (!slot->input.has_value()) {
        slot->input.emplace();
      }
      slot->input->lowering = std::move(lowering).value();
      return detail::ready_graph_sender(
          wh::core::result<graph_value>{graph_value{
              std::move(reader).value()}});
    }
  }

  value_batch base_batch{
      .form = lanes.size() > 1U ? detail::input_runtime::value_input_form::fan_in
                                : detail::input_runtime::value_input_form::direct,
  };
  base_batch.fan_in.reserve(lanes.size());

  std::vector<std::uint32_t> async_edges{};
  async_edges.reserve(lanes.size());
  for (const auto &lane : lanes) {
    const auto &edge = core().compiled_execution_index_.index.indexed_edges[lane.edge_id];
    if (!io_storage.edge_value_valid.test(lane.edge_id) &&
        needs_reader_lowering(edge)) {
      async_edges.push_back(lane.edge_id);
      continue;
    }

    auto edge_output = resolve_edge_value(lane.edge_id, io_storage, context);
    if (edge_output.has_error()) {
      return detail::failure_graph_sender(edge_output.error());
    }
    value_input entry{};
    entry.source_id = lane.source_id;
    entry.edge_id = lane.edge_id;
    entry.borrowed = edge_output.value();
    if (io_storage.edge_value_valid.test(lane.edge_id)) {
      entry.owned.emplace(std::move(io_storage.edge_values[lane.edge_id]));
      io_storage.edge_value_valid.clear(lane.edge_id);
    }
    auto appended =
        detail::input_runtime::append_value_input(base_batch, std::move(entry));
    if (appended.has_error()) {
      return detail::failure_graph_sender(appended.error());
    }
  }

  if (async_edges.empty()) {
    auto resolved = finish_value_input(*node, std::move(base_batch));
    if (resolved.has_error() && resolved.error() == wh::core::errc::not_found) {
      resolved = build_missing_input(*node);
    }
    auto finalized = finalize_input(std::move(resolved));
    if (finalized.has_error()) {
      return detail::failure_graph_sender(finalized.error());
    }
    return detail::ready_graph_sender(std::move(finalized));
  }

  struct input_stage {
    const graph *owner{nullptr};
    const compiled_node *node{nullptr};
    value_batch batch{};
    std::vector<std::uint32_t> edge_ids{};
  };

  input_stage stage{
      .owner = this,
      .node = node,
      .batch = std::move(base_batch),
      .edge_ids = std::move(async_edges),
  };

  std::vector<graph_sender> senders{};
  senders.reserve(stage.edge_ids.size());
  for (const auto edge_id : stage.edge_ids) {
    auto reader = take_edge_reader(edge_id, io_storage);
    if (reader.has_error()) {
      return detail::failure_graph_sender(reader.error());
    }
    const auto &edge = core().compiled_execution_index_.index.indexed_edges[edge_id];
    auto lowering = make_reader_lowering(edge);
    if (lowering.has_error()) {
      return detail::failure_graph_sender(lowering.error());
    }
    senders.push_back(lower_reader(std::move(reader).value(),
                                   std::move(lowering).value(), context,
                                   graph_scheduler));
  }

  return detail::bridge_graph_sender(detail::make_child_batch_sender(
      std::move(senders), std::move(stage),
      [](input_stage &stage, const std::size_t index,
         wh::core::result<graph_value> current) -> wh::core::result<void> {
        if (index >= stage.edge_ids.size()) {
          return wh::core::result<void>::failure(wh::core::errc::internal_error);
        }
        if (current.has_error()) {
          return wh::core::result<void>::failure(current.error());
        }

        value_input entry{};
        entry.source_id =
            stage.owner->core().compiled_execution_index_.index.indexed_edges
                [stage.edge_ids[index]]
                    .from;
        entry.edge_id = stage.edge_ids[index];
        entry.owned.emplace(std::move(current).value());
        return detail::input_runtime::append_value_input(stage.batch,
                                                         std::move(entry));
      },
      [finalize_input](input_stage &&stage) -> wh::core::result<graph_value> {
        auto resolved =
            stage.owner->finish_value_input(*stage.node, std::move(stage.batch));
        if (resolved.has_error() &&
            resolved.error() == wh::core::errc::not_found) {
          resolved = stage.owner->build_missing_input(*stage.node);
        }
        auto finalized = finalize_input(std::move(resolved));
        if (finalized.has_error()) {
          return wh::core::result<graph_value>::failure(finalized.error());
        }
        return std::move(finalized).value();
      },
      graph_scheduler));
}

} // namespace wh::compose
