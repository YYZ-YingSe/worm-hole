// Defines graph runtime input lowering, edge adaptation, and input assembly helpers.
#pragma once

#include "wh/compose/graph/graph.hpp"

namespace wh::compose {
inline auto graph::resolve_edge_status_indexed(
    const indexed_edge &edge, const std::vector<node_state> &node_states,
    const std::vector<branch_state> &branch_states) const
    -> edge_status {
  const auto source_state = node_states[edge.from];
  if (source_state == node_state::pending ||
      source_state == node_state::running) {
    return edge_status::waiting;
  }
  if (source_state == node_state::skipped) {
    return edge_status::disabled;
  }

  const auto *branch = compiled_execution_index_.index.branch_for_source(edge.from);
  if (branch == nullptr || !branch->contains(edge.to)) {
    return edge_status::active;
  }

  const auto &decision = branch_states[edge.from];
  if (!decision.decided) {
    return edge_status::waiting;
  }
  return std::binary_search(decision.selected_end_nodes_sorted.begin(),
                            decision.selected_end_nodes_sorted.end(), edge.to)
             ? edge_status::active
             : edge_status::disabled;
}

inline auto graph::classify_node_readiness_indexed(
    const std::uint32_t node_id, const std::vector<node_state> &node_states,
    const std::vector<branch_state> &branch_states,
    const dynamic_bitset &output_valid,
    const scratch_buffer &scratch_buffer) const -> ready_state {
  std::size_t total_control_edges = 0U;
  std::size_t active_control_edges = 0U;
  std::size_t waiting_control_edges = 0U;
  for (const auto edge_id : compiled_execution_index_.index.incoming_control(node_id)) {
    ++total_control_edges;
    const auto status = resolve_edge_status_indexed(
        compiled_execution_index_.index.indexed_edges[edge_id], node_states, branch_states);
    if (status == edge_status::active) {
      ++active_control_edges;
    } else if (status == edge_status::waiting) {
      ++waiting_control_edges;
    }
  }

  auto ready_by_control = false;
  if (options_.trigger_mode == graph_trigger_mode::all_predecessors) {
    if (waiting_control_edges > 0U) {
      return ready_state::waiting;
    }
    if (total_control_edges > 0U) {
      if (active_control_edges == total_control_edges) {
        ready_by_control = true;
      } else {
        return ready_state::skipped;
      }
    }
  } else {
    if (active_control_edges > 0U) {
      if (waiting_control_edges > 0U && node_id == compiled_execution_index_.index.end_id) {
        return ready_state::waiting;
      }
      ready_by_control = true;
    } else if (waiting_control_edges > 0U) {
      return ready_state::waiting;
    } else if (total_control_edges > 0U) {
      return ready_state::skipped;
    }
  }

  if (!ready_by_control) {
    const auto *node = compiled_execution_index_.index.nodes_by_id[node_id];
    if (node != nullptr && node->meta.options.allow_no_control) {
      ready_by_control = true;
    }
  }
  if (!ready_by_control) {
    return ready_state::skipped;
  }

  if (options_.fan_in_policy != graph_fan_in_policy::allow_partial &&
      node_id != compiled_execution_index_.index.end_id) {
    for (const auto edge_id : compiled_execution_index_.index.incoming_data(node_id)) {
      const auto &edge = compiled_execution_index_.index.indexed_edges[edge_id];
      const auto status =
          resolve_edge_status_indexed(edge, node_states, branch_states);
      if (status == edge_status::waiting) {
        return ready_state::waiting;
      }
      if (status == edge_status::active && !output_valid.test(edge.from)) {
        return ready_state::waiting;
      }
      if (status == edge_status::active &&
          options_.fan_in_policy ==
              graph_fan_in_policy::require_all_sources_with_eof &&
          !is_reader_eof_visible_for_fan_in_input(edge.from, scratch_buffer)) {
        return ready_state::waiting;
      }
    }
  }
  return ready_state::ready;
}

inline auto graph::is_reader_eof_visible_for_fan_in_input(
    const std::uint32_t source_node_id,
    const scratch_buffer &scratch_buffer) const -> bool {
  const auto *node = compiled_execution_index_.index.nodes_by_id[source_node_id];
  if (node == nullptr || node->meta.output_contract != node_contract::stream) {
    return true;
  }
  if (!scratch_buffer.output_valid.test(source_node_id)) {
    return false;
  }
  return scratch_buffer.source_eof_visible.test(source_node_id) ||
         scratch_buffer.node_readers[source_node_id].is_source_closed();
}

inline auto graph::make_reader_lowering(const indexed_edge &edge)
    -> wh::core::result<reader_lowering> {
  if (!needs_reader_lowering(edge)) {
    return wh::core::result<reader_lowering>::failure(
        wh::core::errc::invalid_argument);
  }
  if (edge.adapter.kind == edge_adapter_kind::stream_to_value) {
    return reader_lowering{.limits = edge.limits, .project = nullptr};
  }
  if (edge.adapter.kind == edge_adapter_kind::custom) {
    if (!edge.adapter.custom.stream_to_value.has_value()) {
      return wh::core::result<reader_lowering>::failure(
          wh::core::errc::contract_violation);
    }
    return reader_lowering{
        .limits = edge.limits,
        .project = std::addressof(*edge.adapter.custom.stream_to_value),
    };
  }
  return wh::core::result<reader_lowering>::failure(
      wh::core::errc::contract_violation);
}

inline auto graph::lower_reader(graph_stream_reader reader,
                                reader_lowering lowering,
                                wh::core::run_context &context,
                                const wh::core::detail::any_resume_scheduler_t
                                    &graph_scheduler) -> graph_sender {
  if (lowering.project == nullptr) {
    return collect_reader_value(std::move(reader), lowering.limits,
                                graph_scheduler);
  }
  return detail::bridge_graph_sender(
      wh::core::detail::bind_sender_scheduler(
          (*lowering.project)(std::move(reader), lowering.limits, context),
          wh::core::detail::any_resume_scheduler_t{graph_scheduler}));
}

inline auto graph::needs_reader_copy(const std::uint32_t node_id) const
    noexcept -> bool {
  return node_id < compiled_execution_index_.plan.outputs.size() &&
         compiled_execution_index_.plan.outputs[node_id].reader_edges.size() > 1U;
}

inline auto graph::needs_reader_merge(
    const std::uint32_t node_id) const noexcept -> bool {
  return node_id < compiled_execution_index_.plan.inputs.size() &&
         compiled_execution_index_.plan.inputs[node_id].reader_edges.size() > 1U;
}

inline auto graph::adapt_edge_output(const indexed_edge &edge,
                                     graph_value &source_output,
                                     wh::core::run_context &context) const
    -> wh::core::result<graph_value> {
  switch (edge.adapter.kind) {
  case edge_adapter_kind::none:
    return source_output;
  case edge_adapter_kind::value_to_stream: {
    auto lifted_payload = fork_graph_value(source_output);
    if (lifted_payload.has_error()) {
      return wh::core::result<graph_value>::failure(lifted_payload.error());
    }
    auto lifted = payload_to_reader(std::move(lifted_payload).value());
    if (lifted.has_error()) {
      return wh::core::result<graph_value>::failure(lifted.error());
    }
    return wh::core::any(std::move(lifted).value());
  }
  case edge_adapter_kind::stream_to_value: {
    (void)source_output;
    (void)context;
    // stream->value must be drained by the async input stage first.
    return wh::core::result<graph_value>::failure(wh::core::errc::not_supported);
  }
  case edge_adapter_kind::custom: {
    auto adapted_input = fork_graph_value(source_output);
    if (adapted_input.has_error()) {
      return wh::core::result<graph_value>::failure(adapted_input.error());
    }
    if (edge.source_output == node_contract::value &&
        edge.target_input == node_contract::stream) {
      if (!edge.adapter.custom.value_to_stream.has_value()) {
        return wh::core::result<graph_value>::failure(
            wh::core::errc::contract_violation);
      }
      auto lowered = (*edge.adapter.custom.value_to_stream)(
          std::move(adapted_input).value(), edge.limits, context);
      if (lowered.has_error()) {
        return wh::core::result<graph_value>::failure(lowered.error());
      }
      return wh::core::any(std::move(lowered).value());
    }
    if (edge.source_output == node_contract::stream &&
        edge.target_input == node_contract::value) {
      (void)adapted_input;
      // stream->value must be lowered by the async input stage first.
      return wh::core::result<graph_value>::failure(
          wh::core::errc::not_supported);
    }
    return wh::core::result<graph_value>::failure(
        wh::core::errc::contract_violation);
  }
  }
  return wh::core::result<graph_value>::failure(wh::core::errc::internal_error);
}

inline auto graph::store_node_output(
    const std::uint32_t node_id, scratch_buffer &scratch_buffer,
    graph_value value) const -> wh::core::result<void> {
  const auto *node = compiled_execution_index_.index.nodes_by_id[node_id];
  if (node == nullptr) {
    return wh::core::result<void>::failure(wh::core::errc::not_found);
  }
  if (node->meta.output_contract == node_contract::value) {
    scratch_buffer.mark_value_output(node_id, std::move(value));
    return {};
  }
  auto *reader = wh::core::any_cast<graph_stream_reader>(&value);
  if (reader == nullptr) {
    return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
  }
  const auto source_closed = reader->is_source_closed();
  scratch_buffer.mark_reader_output(node_id, std::move(*reader), source_closed);
  return {};
}

inline auto graph::view_node_output(
    const std::uint32_t node_id, scratch_buffer &scratch_buffer) const
    -> wh::core::result<graph_value> {
  const auto *node = compiled_execution_index_.index.nodes_by_id[node_id];
  if (node == nullptr || !scratch_buffer.output_valid.test(node_id)) {
    return wh::core::result<graph_value>::failure(wh::core::errc::not_found);
  }
  if (node->meta.output_contract == node_contract::value) {
    auto &value = scratch_buffer.node_values[node_id];
    if (value.copyable()) {
      return graph_value{value};
    }
    return value.as_ref();
  }
  return wh::core::any::ref(scratch_buffer.node_readers[node_id]);
}

inline auto graph::cache_node_output(
    const std::uint32_t node_id, scratch_buffer &scratch_buffer) const
    -> wh::core::result<graph_value> {
  const auto *node = compiled_execution_index_.index.nodes_by_id[node_id];
  if (node == nullptr || !scratch_buffer.output_valid.test(node_id)) {
    return wh::core::result<graph_value>::failure(wh::core::errc::not_found);
  }
  if (node->meta.output_contract != node_contract::value) {
    return wh::core::result<graph_value>::failure(
        wh::core::errc::contract_violation);
  }
  return fork_graph_value(scratch_buffer.node_values[node_id]);
}

inline auto graph::take_node_output(
    const std::uint32_t node_id, scratch_buffer &scratch_buffer) const
    -> wh::core::result<graph_value> {
  const auto *node = compiled_execution_index_.index.nodes_by_id[node_id];
  if (node == nullptr || !scratch_buffer.output_valid.test(node_id)) {
    return wh::core::result<graph_value>::failure(wh::core::errc::not_found);
  }
  if (node->meta.output_contract == node_contract::value) {
    return std::move(scratch_buffer.node_values[node_id]);
  }
  return graph_value{std::move(scratch_buffer.node_readers[node_id])};
}

inline auto graph::resolve_edge_value(
    const std::uint32_t edge_id, scratch_buffer &scratch_buffer,
    wh::core::run_context &context) const
    -> wh::core::result<graph_value *> {
  if (edge_id >= compiled_execution_index_.index.indexed_edges.size() ||
      edge_id >= scratch_buffer.edge_values.size()) {
    return wh::core::result<graph_value *>::failure(
        wh::core::errc::not_found);
  }
  if (scratch_buffer.edge_value_valid.test(edge_id)) {
    return std::addressof(scratch_buffer.edge_values[edge_id]);
  }

  const auto &edge = compiled_execution_index_.index.indexed_edges[edge_id];
  if (!scratch_buffer.output_valid.test(edge.from)) {
    return wh::core::result<graph_value *>::failure(
        wh::core::errc::not_found);
  }
  if (edge.target_input != node_contract::value ||
      needs_reader_lowering(edge)) {
    return wh::core::result<graph_value *>::failure(
        wh::core::errc::not_supported);
  }

  if (edge.source_output == node_contract::value) {
    if (edge.adapter.kind == edge_adapter_kind::none) {
      return std::addressof(scratch_buffer.node_values[edge.from]);
    }
    return wh::core::result<graph_value *>::failure(
        wh::core::errc::contract_violation);
  }

  if (edge.adapter.kind != edge_adapter_kind::custom) {
    return wh::core::result<graph_value *>::failure(
        wh::core::errc::contract_violation);
  }
  auto reader = take_edge_reader(edge_id, scratch_buffer, context);
  if (reader.has_error()) {
    return wh::core::result<graph_value *>::failure(reader.error());
  }
  auto stream_input = wh::core::any(std::move(reader).value());
  auto adapted = adapt_edge_output(edge, stream_input, context);
  if (adapted.has_error()) {
    return wh::core::result<graph_value *>::failure(adapted.error());
  }
  scratch_buffer.edge_values[edge_id] = std::move(adapted).value();
  scratch_buffer.edge_value_valid.set(edge_id);
  return std::addressof(scratch_buffer.edge_values[edge_id]);
}

inline auto graph::prepare_reader_copies(
    const std::uint32_t source_node_id,
    scratch_buffer &scratch_buffer) const -> wh::core::result<void> {
  if (!needs_reader_copy(source_node_id) ||
      scratch_buffer.reader_copy_ready.test(source_node_id)) {
    return {};
  }

  if (!scratch_buffer.output_valid.test(source_node_id)) {
    return wh::core::result<void>::failure(wh::core::errc::not_found);
  }
  auto &source_reader = scratch_buffer.node_readers[source_node_id];
  const auto &reader_edges = compiled_execution_index_.plan.outputs[source_node_id].reader_edges;
  auto copied_readers =
      detail::copy_graph_readers(std::move(source_reader), reader_edges.size() + 1U);
  if (copied_readers.has_error()) {
    return wh::core::result<void>::failure(copied_readers.error());
  }
  auto readers = std::move(copied_readers).value();
  if (readers.size() != reader_edges.size() + 1U) {
    return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
  }

  source_reader = std::move(readers.front());
  for (std::size_t index = 0U; index < reader_edges.size(); ++index) {
    const auto edge_id = reader_edges[index];
    scratch_buffer.edge_readers[edge_id] = std::move(readers[index + 1U]);
    scratch_buffer.edge_reader_valid.set(edge_id);
  }
  scratch_buffer.reader_copy_ready.set(source_node_id);
  return {};
}

inline auto graph::resolve_edge_reader(
    const std::uint32_t edge_id, scratch_buffer &scratch_buffer,
    wh::core::run_context &context) const
    -> wh::core::result<graph_stream_reader *> {
  if (edge_id >= compiled_execution_index_.index.indexed_edges.size() ||
      edge_id >= scratch_buffer.edge_readers.size()) {
    return wh::core::result<graph_stream_reader *>::failure(
        wh::core::errc::not_found);
  }
  if (scratch_buffer.edge_reader_valid.test(edge_id)) {
    return std::addressof(scratch_buffer.edge_readers[edge_id]);
  }

  const auto &edge = compiled_execution_index_.index.indexed_edges[edge_id];
  if (!scratch_buffer.output_valid.test(edge.from)) {
    return wh::core::result<graph_stream_reader *>::failure(
        wh::core::errc::not_found);
  }
  if (edge.target_input != node_contract::stream) {
    return wh::core::result<graph_stream_reader *>::failure(
        wh::core::errc::type_mismatch);
  }

  if (edge.source_output == node_contract::stream) {
    if (edge.adapter.kind != edge_adapter_kind::none) {
      return wh::core::result<graph_stream_reader *>::failure(
          wh::core::errc::contract_violation);
    }
    if (needs_reader_copy(edge.from)) {
      auto prepared = prepare_reader_copies(edge.from, scratch_buffer);
      if (prepared.has_error()) {
        return wh::core::result<graph_stream_reader *>::failure(prepared.error());
      }
      if (!scratch_buffer.edge_reader_valid.test(edge_id)) {
        return wh::core::result<graph_stream_reader *>::failure(
            wh::core::errc::not_found);
      }
      return std::addressof(scratch_buffer.edge_readers[edge_id]);
    }
    return std::addressof(scratch_buffer.node_readers[edge.from]);
  }

  if (edge.adapter.kind != edge_adapter_kind::value_to_stream &&
      edge.adapter.kind != edge_adapter_kind::custom) {
    return wh::core::result<graph_stream_reader *>::failure(
        wh::core::errc::contract_violation);
  }

  auto adapted = adapt_edge_output(edge, scratch_buffer.node_values[edge.from],
                                   context);
  if (adapted.has_error()) {
    return wh::core::result<graph_stream_reader *>::failure(adapted.error());
  }
  auto *reader = wh::core::any_cast<graph_stream_reader>(&adapted.value());
  if (reader == nullptr) {
    return wh::core::result<graph_stream_reader *>::failure(
        wh::core::errc::type_mismatch);
  }
  scratch_buffer.edge_readers[edge_id] = std::move(*reader);
  scratch_buffer.edge_reader_valid.set(edge_id);
  return std::addressof(scratch_buffer.edge_readers[edge_id]);
}

inline auto graph::take_edge_reader(
    const std::uint32_t edge_id, scratch_buffer &scratch_buffer,
    wh::core::run_context &context) const
    -> wh::core::result<graph_stream_reader> {
  if (edge_id >= compiled_execution_index_.index.indexed_edges.size()) {
    return wh::core::result<graph_stream_reader>::failure(
        wh::core::errc::not_found);
  }

  const auto &edge = compiled_execution_index_.index.indexed_edges[edge_id];
  if (edge.source_output == node_contract::stream) {
    if (needs_reader_copy(edge.from)) {
      auto prepared = prepare_reader_copies(edge.from, scratch_buffer);
      if (prepared.has_error()) {
        return wh::core::result<graph_stream_reader>::failure(prepared.error());
      }

      if (!scratch_buffer.edge_reader_valid.test(edge_id)) {
        return wh::core::result<graph_stream_reader>::failure(
            wh::core::errc::not_found);
      }
      auto reader = std::move(scratch_buffer.edge_readers[edge_id]);
      scratch_buffer.edge_reader_valid.clear(edge_id);
      return reader;
    }
    return std::move(scratch_buffer.node_readers[edge.from]);
  }

  auto resolved = resolve_edge_reader(edge_id, scratch_buffer, context);
  if (resolved.has_error()) {
    return wh::core::result<graph_stream_reader>::failure(resolved.error());
  }
  auto reader = std::move(*resolved.value());
  scratch_buffer.edge_reader_valid.clear(edge_id);
  return reader;
}

inline auto graph::merged_reader(
    const std::uint32_t node_id, scratch_buffer &scratch_buffer) const
    -> wh::core::result<graph_stream_reader *> {
  if (!needs_reader_merge(node_id)) {
    return wh::core::result<graph_stream_reader *>::failure(
        wh::core::errc::invalid_argument);
  }
  if (!scratch_buffer.merged_reader_valid.test(node_id)) {
    std::vector<std::string> sources{};
    const auto &reader_edges = compiled_execution_index_.plan.inputs[node_id].reader_edges;
    sources.reserve(reader_edges.size());
    for (const auto edge_id : reader_edges) {
      sources.push_back(
          compiled_execution_index_.index.id_to_key[compiled_execution_index_.index.indexed_edges[edge_id].from]);
    }
    scratch_buffer.merged_readers[node_id] =
        detail::make_graph_merge_reader(std::move(sources));
    scratch_buffer.merged_reader_valid.set(node_id);
  }
  return std::addressof(scratch_buffer.merged_readers[node_id]);
}

inline auto graph::update_merged_reader(
    const std::uint32_t node_id, scratch_buffer &scratch_buffer,
    const std::vector<input_lane> &lanes, wh::core::run_context &context) const
    -> wh::core::result<void> {
  if (!needs_reader_merge(node_id)) {
    return {};
  }

  auto merged = merged_reader(node_id, scratch_buffer);
  if (merged.has_error()) {
    return wh::core::result<void>::failure(merged.error());
  }
  auto *shell =
      merged.value()
          ->template target_if<
              wh::schema::stream::merge_stream_reader<graph_stream_reader>>();
  if (shell == nullptr) {
    return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
  }

  for (const auto &lane : lanes) {
    auto &lane_state = scratch_buffer.merged_reader_lane_states[lane.edge_id];
    if (lane_state != reader_lane_state::unseen) {
      continue;
    }
    if (lane.status == edge_status::waiting ||
        (lane.status == edge_status::active && !lane.output_ready)) {
      continue;
    }

    const auto &source_key = compiled_execution_index_.index.id_to_key[lane.source_id];
    if (lane.status == edge_status::active) {
      auto attached_reader = take_edge_reader(lane.edge_id, scratch_buffer, context);
      if (attached_reader.has_error()) {
        return wh::core::result<void>::failure(attached_reader.error());
      }
      auto attached = shell->attach(source_key, std::move(attached_reader).value());
      if (attached.has_error()) {
        return wh::core::result<void>::failure(attached.error());
      }
      lane_state = reader_lane_state::attached;
      continue;
    }

    auto disabled = shell->disable(source_key);
    if (disabled.has_error()) {
      return wh::core::result<void>::failure(disabled.error());
    }
    lane_state = reader_lane_state::disabled;
  }
  return {};
}

inline auto graph::refresh_merged_reader(
    const std::uint32_t node_id, scratch_buffer &scratch_buffer,
    const std::vector<node_state> &node_states,
    const std::vector<branch_state> &branch_states,
    wh::core::run_context &context) const -> wh::core::result<void> {
  const auto lanes =
      collect_input_lanes(node_id, node_states, branch_states,
                          scratch_buffer.output_valid);
  return update_merged_reader(node_id, scratch_buffer, lanes, context);
}

inline auto graph::build_reader_input(
    const compiled_node &node, const std::uint32_t node_id,
    scratch_buffer &scratch_buffer, const std::vector<input_lane> &lanes,
    wh::core::run_context &context) const
    -> wh::core::result<resolved_input> {
  if (needs_reader_merge(node_id)) {
    auto synced = update_merged_reader(node_id, scratch_buffer, lanes, context);
    if (synced.has_error()) {
      return wh::core::result<resolved_input>::failure(synced.error());
    }
    return resolved_input::borrow_reader(scratch_buffer.merged_readers[node_id]);
  }

  for (std::size_t offset = lanes.size(); offset > 0U; --offset) {
    const auto &lane = lanes[offset - 1U];
    if (lane.status != edge_status::active || !lane.output_ready) {
      continue;
    }

    auto reader = take_edge_reader(lane.edge_id, scratch_buffer, context);
    if (reader.has_error()) {
      return wh::core::result<resolved_input>::failure(reader.error());
    }
    return resolved_input::own_reader(std::move(reader).value());
  }

  return build_missing_input(node, !lanes.empty(), scratch_buffer);
}

inline auto graph::build_value_input(
    const compiled_node &node, const bool has_data_edge,
    scratch_buffer &scratch_buffer, const std::vector<input_lane> &lanes,
    graph_value &scratch, wh::core::run_context &context,
    const detail::runtime_state::invoke_config &config) const
    -> wh::core::result<resolved_input> {
  value_batch batch{
      .has_data_edge = has_data_edge,
      .has_static_fan_in = lanes.size() > 1U,
  };
  batch.active.reserve(lanes.size());
  const auto append_input = [&batch](value_input entry) -> void {
    if (batch.has_static_fan_in) {
      batch.active.push_back(std::move(entry));
      return;
    }
    if (batch.single.has_value()) {
      batch.active.reserve(2U);
      batch.active.push_back(std::move(*batch.single));
      batch.single.reset();
      batch.active.push_back(std::move(entry));
      return;
    }
    batch.single = std::move(entry);
  };

  for (const auto &lane : lanes) {
    if (lane.status != edge_status::active) {
      continue;
    }
    if (!lane.output_ready) {
      if (options_.fan_in_policy != graph_fan_in_policy::allow_partial) {
        return wh::core::result<resolved_input>::failure(
            wh::core::errc::not_found);
      }
      continue;
    }

    auto edge_output = resolve_edge_value(lane.edge_id, scratch_buffer, context);
    if (edge_output.has_error()) {
      return wh::core::result<resolved_input>::failure(edge_output.error());
    }
    value_input entry{};
    entry.source_id = lane.source_id;
    entry.edge_id = lane.edge_id;
    entry.borrowed = edge_output.value();
    if (scratch_buffer.edge_value_valid.test(lane.edge_id)) {
      entry.owned.emplace(std::move(scratch_buffer.edge_values[lane.edge_id]));
      scratch_buffer.edge_value_valid.clear(lane.edge_id);
    }
    append_input(std::move(entry));
  }

  auto resolved = finish_value_input(node, std::move(batch), scratch, config);
  if (resolved.has_error() && resolved.error() == wh::core::errc::not_found) {
    return build_missing_input(node, has_data_edge, scratch_buffer);
  }
  return resolved;
}

inline auto graph::finish_value_input(
    const compiled_node &node, value_batch batch, graph_value &scratch,
    const detail::runtime_state::invoke_config &config) const
    -> wh::core::result<resolved_input> {
  (void)node;

  if (options_.fan_in_policy == graph_fan_in_policy::allow_partial) {
    if (batch.has_static_fan_in && !batch.active.empty()) {
      std::vector<const graph_value *> merge_inputs{};
      merge_inputs.reserve(batch.active.size());
      for (const auto &entry : batch.active) {
        merge_inputs.push_back(entry.value());
      }

      auto merged = merge_value_inputs(merge_inputs, config, scratch);
      if (merged.has_error()) {
        return wh::core::result<resolved_input>::failure(merged.error());
      }
      if (merged.value()) {
        return resolved_input::own_value(std::move(scratch));
      }

      graph_value_map fan_in_input{};
      fan_in_input.reserve(batch.active.size());
      for (const auto &entry : batch.active) {
        fan_in_input.insert_or_assign(compiled_execution_index_.index.id_to_key[entry.source_id],
                                      *entry.value());
      }
      return resolved_input::own_value(wh::core::any(std::move(fan_in_input)));
    }
    if (batch.single.has_value()) {
      if (batch.single->owned.has_value()) {
        return resolved_input::own_value(
            std::move(*batch.single->owned));
      }
      return resolved_input::borrow_value(*batch.single->value());
    }
    return wh::core::result<resolved_input>::failure(
        wh::core::errc::not_found);
  }

  if (batch.single.has_value()) {
    if (batch.single->owned.has_value()) {
      return resolved_input::own_value(std::move(*batch.single->owned));
    }
    return resolved_input::borrow_value(*batch.single->value());
  }

  if (batch.active.empty()) {
    return wh::core::result<resolved_input>::failure(
        wh::core::errc::not_found);
  }

  std::vector<const graph_value *> merge_inputs{};
  merge_inputs.reserve(batch.active.size());
  for (const auto &entry : batch.active) {
    merge_inputs.push_back(entry.value());
  }

  auto merged = merge_value_inputs(merge_inputs, config, scratch);
  if (merged.has_error()) {
    return wh::core::result<resolved_input>::failure(merged.error());
  }
  if (merged.value()) {
    return resolved_input::own_value(std::move(scratch));
  }

  graph_value_map fan_in_input{};
  fan_in_input.reserve(batch.active.size());
  for (const auto &entry : batch.active) {
    fan_in_input.insert_or_assign(compiled_execution_index_.index.id_to_key[entry.source_id],
                                  *entry.value());
  }
  return resolved_input::own_value(wh::core::any(std::move(fan_in_input)));
}

inline auto graph::refresh_source_readers(
    const std::uint32_t source_node_id, scratch_buffer &scratch_buffer,
    const std::vector<node_state> &node_states,
    const std::vector<branch_state> &branch_states,
    wh::core::run_context &context) const -> wh::core::result<void> {
  if (scratch_buffer.output_valid.test(source_node_id) &&
      scratch_buffer.node_readers[source_node_id].is_source_closed()) {
    scratch_buffer.source_eof_visible.set(source_node_id);
  }
  for (const auto edge_id : compiled_execution_index_.plan.outputs[source_node_id].reader_edges) {
    const auto target_id = compiled_execution_index_.index.indexed_edges[edge_id].to;
    if (!needs_reader_merge(target_id) ||
        !scratch_buffer.merged_reader_valid.test(target_id)) {
      continue;
    }
    auto refreshed = refresh_merged_reader(
        target_id, scratch_buffer, node_states, branch_states, context);
    if (refreshed.has_error()) {
      return refreshed;
    }
  }
  return {};
}

inline auto graph::merge_value_inputs(
    const std::vector<const graph_value *> &active_inputs,
    const detail::runtime_state::invoke_config &config, graph_value &scratch) const
    -> wh::core::result<bool> {
  if (active_inputs.empty()) {
    return false;
  }

  for (const auto *value : active_inputs) {
    if (value == nullptr) {
      return wh::core::result<bool>::failure(wh::core::errc::not_found);
    }
    if (wh::core::any_cast<graph_stream_reader>(value) != nullptr) {
      return wh::core::result<bool>::failure(wh::core::errc::type_mismatch);
    }
  }

  if (config.values_merge_registry != nullptr) {
    wh::core::any_type_key input_type{};
    wh::internal::dynamic_merge_values dynamic_inputs_storage{};
    std::vector<wh::internal::dynamic_merge_value> dynamic_values{};
    dynamic_values.reserve(active_inputs.size());
    for (const auto *value : active_inputs) {
      if (value == nullptr) {
        return wh::core::result<bool>::failure(wh::core::errc::not_found);
      }
      const auto current_type = value->key();
      if (input_type.token == nullptr) {
        input_type = current_type;
      } else if (input_type != current_type) {
        return wh::core::result<bool>::failure(wh::core::errc::type_mismatch);
      }
      dynamic_values.push_back(value->as_ref());
    }
    dynamic_inputs_storage = wh::internal::dynamic_merge_values{
        dynamic_values.data(), dynamic_values.size()};
    auto merged =
        values_merge(*config.values_merge_registry, input_type, dynamic_inputs_storage);
    if (merged.has_error()) {
      return wh::core::result<bool>::failure(merged.error());
    }
    scratch = graph_value{std::move(merged).value()};
    return true;
  }
  return false;
}

inline auto graph::collect_input_lanes(
    const std::uint32_t node_id,
    const std::vector<node_state> &node_states,
    const std::vector<branch_state> &branch_states,
    const dynamic_bitset &output_valid) const -> std::vector<input_lane> {
  std::vector<input_lane> lanes{};
  const auto incoming = compiled_execution_index_.index.incoming_data(node_id);
  lanes.reserve(incoming.size());
  for (const auto edge_id : incoming) {
    const auto &edge = compiled_execution_index_.index.indexed_edges[edge_id];
    const auto status =
        resolve_edge_status_indexed(edge, node_states, branch_states);
    lanes.push_back(input_lane{
        .edge_id = edge_id,
        .source_id = edge.from,
        .status = status,
        .output_ready =
            status == edge_status::active && output_valid.test(edge.from),
    });
  }
  return lanes;
}

inline auto graph::build_missing_input(const compiled_node &node,
                                       const bool has_data_edge,
                                       scratch_buffer &scratch_buffer) const
    -> wh::core::result<resolved_input> {
  if (node.meta.options.allow_no_data) {
    auto missing = make_missing_rerun_input_default(node.meta.input_contract);
    if (missing.has_error()) {
      return wh::core::result<resolved_input>::failure(missing.error());
    }
    return own_input(std::move(missing).value(), node.meta.input_contract);
  }
  if (!has_data_edge &&
      scratch_buffer.output_valid.test(compiled_execution_index_.index.start_id)) {
    if (node.meta.input_contract == node_contract::stream) {
      return resolved_input::borrow_reader(
          scratch_buffer.node_readers[compiled_execution_index_.index.start_id]);
    }
    return resolved_input::borrow_value(
        scratch_buffer.node_values[compiled_execution_index_.index.start_id]);
  }
  return wh::core::result<resolved_input>::failure(
      wh::core::errc::not_found);
}

inline auto graph::borrow_input(graph_value &value,
                                const node_contract contract)
    -> wh::core::result<resolved_input> {
  if (contract == node_contract::stream) {
    auto *reader = wh::core::any_cast<graph_stream_reader>(&value);
    if (reader == nullptr) {
      return wh::core::result<resolved_input>::failure(
          wh::core::errc::type_mismatch);
    }
    return resolved_input::borrow_reader(*reader);
  }
  return resolved_input::borrow_value(value);
}

inline auto graph::own_input(graph_value value, const node_contract contract)
    -> wh::core::result<resolved_input> {
  if (contract == node_contract::stream) {
    auto *reader = wh::core::any_cast<graph_stream_reader>(&value);
    if (reader == nullptr) {
      return wh::core::result<resolved_input>::failure(
          wh::core::errc::type_mismatch);
    }
    return resolved_input::own_reader(std::move(*reader));
  }
  return resolved_input::own_value(std::move(value));
}

inline auto graph::build_node_input(
    const std::uint32_t node_id, scratch_buffer &scratch_buffer,
    const std::vector<node_state> &node_states,
    const std::vector<branch_state> &branch_states,
    graph_value &scratch, wh::core::run_context &context,
    const detail::runtime_state::invoke_config &config) const
    -> wh::core::result<resolved_input> {
  const auto *node = compiled_execution_index_.index.nodes_by_id[node_id];
  if (node == nullptr) {
    return wh::core::result<resolved_input>::failure(
        wh::core::errc::not_found);
  }

  const auto lanes =
      collect_input_lanes(node_id, node_states, branch_states,
                          scratch_buffer.output_valid);
  const bool has_data_edge = !lanes.empty();

  if (node->meta.input_contract == node_contract::stream) {
    return build_reader_input(*node, node_id, scratch_buffer, lanes, context);
  }
  return build_value_input(*node, has_data_edge, scratch_buffer, lanes, scratch,
                           context, config);
}

inline auto graph::build_node_input_sender(
    const std::uint32_t node_id, scratch_buffer &scratch_buffer,
    const std::vector<node_state> &node_states,
    const std::vector<branch_state> &branch_states,
    wh::core::run_context &context, node_frame *frame,
    const detail::runtime_state::invoke_config &config,
    const wh::core::detail::any_resume_scheduler_t &graph_scheduler) const
    -> graph_sender {
  const auto *node = compiled_execution_index_.index.nodes_by_id[node_id];
  if (node == nullptr) {
    return detail::failure_graph_sender(wh::core::errc::not_found);
  }

  const auto finalize_input =
      [this, node, &context](wh::core::result<resolved_input> resolved)
      -> wh::core::result<graph_value> {
    if (resolved.has_error() && resolved.error() == wh::core::errc::not_found &&
        context.resume_info.has_value()) {
      auto fallback = resolve_missing_rerun_input(node->meta.input_contract);
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
    return std::move(resolved).value().materialize();
  };

  if (node->meta.input_contract == node_contract::stream) {
    graph_value scratch{};
    auto resolved = build_node_input(
        node_id, scratch_buffer, node_states, branch_states, scratch, context,
        config);
    auto finalized = finalize_input(std::move(resolved));
    if (finalized.has_error()) {
      return detail::failure_graph_sender(finalized.error());
    }
    return detail::ready_graph_sender(std::move(finalized));
  }

  const auto lanes =
      collect_input_lanes(node_id, node_states, branch_states,
                          scratch_buffer.output_valid);
  const bool preserve_stream_pre =
      frame != nullptr &&
      detail::state_runtime::has_stream_phase(
          frame->state_handlers, detail::state_runtime::state_phase::pre);
  if (preserve_stream_pre) {
    std::vector<std::uint32_t> active_edges{};
    active_edges.reserve(lanes.size());
    for (const auto &lane : lanes) {
      if (lane.status != edge_status::active || !lane.output_ready) {
        continue;
      }
      active_edges.push_back(lane.edge_id);
    }

    if (active_edges.size() == 1U) {
      const auto edge_id = active_edges.front();
      const auto &edge = compiled_execution_index_.index.indexed_edges[edge_id];
      if (needs_reader_lowering(edge)) {
        auto reader = take_edge_reader(edge_id, scratch_buffer, context);
        if (reader.has_error()) {
          return detail::failure_graph_sender(reader.error());
        }
        auto lowering = make_reader_lowering(edge);
        if (lowering.has_error()) {
          return detail::failure_graph_sender(lowering.error());
        }
        frame->input_lowering = std::move(lowering).value();
        return detail::ready_graph_sender(
            wh::core::result<graph_value>{wh::core::any(std::move(reader).value())});
      }
    }
  }

  value_batch base_batch{
      .has_data_edge = !lanes.empty(),
      .has_static_fan_in = lanes.size() > 1U,
  };
  base_batch.active.reserve(lanes.size());
  const auto append_input = [&](value_input entry) -> void {
    if (base_batch.has_static_fan_in) {
      base_batch.active.push_back(std::move(entry));
      return;
    }
    if (base_batch.single.has_value()) {
      base_batch.active.reserve(2U);
      base_batch.active.push_back(std::move(*base_batch.single));
      base_batch.single.reset();
      base_batch.active.push_back(std::move(entry));
      return;
    }
    base_batch.single = std::move(entry);
  };

  std::vector<std::uint32_t> async_edges{};
  async_edges.reserve(lanes.size());
  bool blocked = false;
  for (const auto &lane : lanes) {
    if (lane.status != edge_status::active) {
      continue;
    }
    if (!lane.output_ready) {
      if (options_.fan_in_policy != graph_fan_in_policy::allow_partial) {
        blocked = true;
        break;
      }
      continue;
    }

    const auto &edge = compiled_execution_index_.index.indexed_edges[lane.edge_id];
    if (!scratch_buffer.edge_value_valid.test(lane.edge_id) &&
        needs_reader_lowering(edge)) {
      async_edges.push_back(lane.edge_id);
      continue;
    }

    auto edge_output = resolve_edge_value(lane.edge_id, scratch_buffer, context);
    if (edge_output.has_error()) {
      return detail::failure_graph_sender(edge_output.error());
    }
    value_input entry{};
    entry.source_id = lane.source_id;
    entry.edge_id = lane.edge_id;
    entry.borrowed = edge_output.value();
    if (scratch_buffer.edge_value_valid.test(lane.edge_id)) {
      entry.owned.emplace(std::move(scratch_buffer.edge_values[lane.edge_id]));
      scratch_buffer.edge_value_valid.clear(lane.edge_id);
    }
    append_input(std::move(entry));
  }

  if (async_edges.empty()) {
    graph_value scratch{};
    auto resolved = blocked
                        ? wh::core::result<resolved_input>::failure(
                              wh::core::errc::not_found)
                        : finish_value_input(*node, std::move(base_batch), scratch,
                                             config);
    if (resolved.has_error() && resolved.error() == wh::core::errc::not_found) {
      resolved = build_missing_input(*node, !lanes.empty(), scratch_buffer);
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
    bool blocked{false};
    const detail::runtime_state::invoke_config *config{nullptr};
    std::vector<std::uint32_t> edge_ids{};
    graph_value scratch_value{};
  };

  input_stage stage{
      .owner = this,
      .node = node,
      .batch = std::move(base_batch),
      .blocked = blocked,
      .config = std::addressof(config),
      .edge_ids = std::move(async_edges),
  };

  std::vector<graph_sender> senders{};
  senders.reserve(stage.edge_ids.size());
  for (const auto edge_id : stage.edge_ids) {
    auto reader = take_edge_reader(edge_id, scratch_buffer, context);
    if (reader.has_error()) {
      return detail::failure_graph_sender(reader.error());
    }
    const auto &edge = compiled_execution_index_.index.indexed_edges[edge_id];
    auto lowering = make_reader_lowering(edge);
    if (lowering.has_error()) {
      return detail::failure_graph_sender(lowering.error());
    }
    senders.push_back(lower_reader(std::move(reader).value(),
                                   std::move(lowering).value(), context,
                                   graph_scheduler));
  }

  return detail::bridge_graph_sender(
      wh::core::detail::make_concurrent_sender_vector<
          wh::core::result<graph_value>>(std::move(senders), 1U) |
      stdexec::let_value(
          [stage = std::move(stage), finalize_input](
              std::vector<wh::core::result<graph_value>> collected) mutable
              -> graph_sender {
            if (collected.size() != stage.edge_ids.size()) {
              return detail::failure_graph_sender(
                  wh::core::errc::internal_error);
            }

            auto append_input = [&](value_input entry) -> void {
              if (stage.batch.has_static_fan_in) {
                stage.batch.active.push_back(std::move(entry));
                return;
              }
              if (stage.batch.single.has_value()) {
                stage.batch.active.reserve(2U);
                stage.batch.active.push_back(std::move(*stage.batch.single));
                stage.batch.single.reset();
                stage.batch.active.push_back(std::move(entry));
                return;
              }
              stage.batch.single = std::move(entry);
            };

            for (std::size_t index = 0U; index < collected.size(); ++index) {
              if (collected[index].has_error()) {
                return detail::failure_graph_sender(collected[index].error());
              }
              value_input entry{};
              entry.source_id = stage.owner->compiled_execution_index_.index.indexed_edges
                                     [stage.edge_ids[index]]
                                         .from;
              entry.edge_id = stage.edge_ids[index];
              entry.owned.emplace(std::move(collected[index]).value());
              append_input(std::move(entry));
            }

            auto resolved =
                stage.blocked
                    ? wh::core::result<resolved_input>::failure(
                          wh::core::errc::not_found)
                    : stage.owner->finish_value_input(
                          *stage.node, std::move(stage.batch), stage.scratch_value,
                          *stage.config);
            auto finalized = finalize_input(std::move(resolved));
            if (finalized.has_error()) {
              return detail::failure_graph_sender(finalized.error());
            }
            return detail::ready_graph_sender(std::move(finalized));
          }));
}


} // namespace wh::compose
