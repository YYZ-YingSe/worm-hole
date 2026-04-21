// Defines DAG/runtime input lowering, edge adaptation, and input assembly helpers.
#pragma once

#include "wh/compose/graph/graph.hpp"

namespace wh::compose {
inline auto graph::resolve_edge_status_indexed(const indexed_edge &edge,
                                               const std::vector<dag_node_phase> &dag_node_phases,
                                               const std::vector<branch_state> &branch_states) const
    -> edge_status {
  const auto source_state = dag_node_phases[edge.from];
  if (source_state == dag_node_phase::pending || source_state == dag_node_phase::running) {
    return edge_status::waiting;
  }
  if (source_state == dag_node_phase::skipped) {
    return edge_status::disabled;
  }

  const auto *value_branch =
      core().compiled_execution_index_.index.value_branch_for_source(edge.from);
  const auto *stream_branch =
      core().compiled_execution_index_.index.stream_branch_for_source(edge.from);
  const bool guarded_by_branch = (value_branch != nullptr && value_branch->contains(edge.to)) ||
                                 (stream_branch != nullptr && stream_branch->contains(edge.to));
  if (!guarded_by_branch) {
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

inline auto graph::classify_node_readiness_indexed(const std::uint32_t node_id,
                                                   const std::vector<dag_node_phase> &dag_node_phases,
                                                   const std::vector<branch_state> &branch_states,
                                                   const dynamic_bitset &output_valid) const
    -> ready_state {
  const auto &index = core().compiled_execution_index_.index;
  std::size_t total_control_edges = 0U;
  std::size_t active_control_edges = 0U;
  std::size_t waiting_control_edges = 0U;
  for (const auto edge_id : index.incoming_control(node_id)) {
    ++total_control_edges;
    const auto status =
        resolve_edge_status_indexed(index.indexed_edges[edge_id], dag_node_phases, branch_states);
    if (status == edge_status::active) {
      ++active_control_edges;
    } else if (status == edge_status::waiting) {
      ++waiting_control_edges;
    }
  }

  auto ready_by_control = false;
  if (core().options_.trigger_mode == graph_trigger_mode::all_predecessors) {
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
      if (waiting_control_edges > 0U && node_id == index.end_id) {
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
    const auto *node = core().compiled_execution_index_.index.nodes_by_id[node_id];
    if (node != nullptr && node->meta.options.allow_no_control) {
      ready_by_control = true;
    }
  }
  if (!ready_by_control) {
    return ready_state::skipped;
  }

  if (core().options_.fan_in_policy != graph_fan_in_policy::allow_partial &&
      node_id != index.end_id) {
    for (const auto edge_id : index.incoming_data(node_id)) {
      const auto &edge = index.indexed_edges[edge_id];
      const auto status = resolve_edge_status_indexed(edge, dag_node_phases, branch_states);
      if (status == edge_status::waiting) {
        return ready_state::waiting;
      }
      if (status == edge_status::active && !output_valid.test(edge.from)) {
        return ready_state::waiting;
      }
    }
  }
  return ready_state::ready;
}

[[nodiscard]] inline auto compiled_value_input_gate(const compiled_node &node) noexcept
    -> input_gate {
  if (node.meta.compiled_input_gate.has_value()) {
    return *node.meta.compiled_input_gate;
  }
  return node.meta.input_contract == node_contract::stream ? input_gate::reader()
                                                           : input_gate::open();
}

inline auto graph::make_reader_lowering(const indexed_edge &edge)
    -> wh::core::result<reader_lowering> {
  if (!needs_reader_lowering(edge)) {
    return wh::core::result<reader_lowering>::failure(wh::core::errc::invalid_argument);
  }
  if (edge.lowering_kind == edge_lowering_kind::stream_to_value) {
    return reader_lowering{.limits = edge.limits, .project = nullptr};
  }
  if (edge.lowering_kind == edge_lowering_kind::custom) {
    if (!edge.adapter.custom.to_value.has_value()) {
      return wh::core::result<reader_lowering>::failure(wh::core::errc::contract_violation);
    }
    return reader_lowering{
        .limits = edge.limits,
        .project = std::addressof(*edge.adapter.custom.to_value),
    };
  }
  return wh::core::result<reader_lowering>::failure(wh::core::errc::contract_violation);
}

inline auto graph::lower_reader(graph_stream_reader reader, reader_lowering lowering,
                                wh::core::run_context &context,
                                const wh::core::detail::any_resume_scheduler_t &graph_scheduler)
    -> graph_sender {
  if (lowering.project == nullptr) {
    return collect_reader_value(std::move(reader), lowering.limits, graph_scheduler);
  }
  return detail::bridge_graph_sender(wh::core::detail::write_sender_scheduler(
      (*lowering.project)(std::move(reader), lowering.limits, context), graph_scheduler));
}

inline auto graph::needs_reader_merge(const std::uint32_t node_id) const noexcept -> bool {
  return node_id < core().compiled_execution_index_.plan.inputs.size() &&
         core().compiled_execution_index_.plan.inputs[node_id].reader_edges.size() > 1U;
}

inline auto graph::adapt_edge_output(const indexed_edge &edge, graph_value &source_output,
                                     wh::core::run_context &context) const
    -> wh::core::result<graph_value> {
  switch (edge.lowering_kind) {
  case edge_lowering_kind::none:
    return detail::materialize_value_payload(source_output);
  case edge_lowering_kind::value_to_stream: {
    auto lifted_payload = fork_graph_value(source_output);
    if (lifted_payload.has_error()) {
      return wh::core::result<graph_value>::failure(lifted_payload.error());
    }
    auto lifted = make_single_value_stream_reader(std::move(lifted_payload).value());
    if (lifted.has_error()) {
      return wh::core::result<graph_value>::failure(lifted.error());
    }
    return wh::core::any(std::move(lifted).value());
  }
  case edge_lowering_kind::stream_to_value: {
    // stream->value must be drained by the async input stage first.
    return wh::core::result<graph_value>::failure(wh::core::errc::not_supported);
  }
  case edge_lowering_kind::custom: {
    auto adapted_input = fork_graph_value(source_output);
    if (adapted_input.has_error()) {
      return wh::core::result<graph_value>::failure(adapted_input.error());
    }
    if (edge.source_output == node_contract::value && edge.target_input == node_contract::stream) {
      if (!edge.adapter.custom.to_stream.has_value()) {
        return wh::core::result<graph_value>::failure(wh::core::errc::contract_violation);
      }
      auto lowered =
          (*edge.adapter.custom.to_stream)(std::move(adapted_input).value(), edge.limits, context);
      if (lowered.has_error()) {
        return wh::core::result<graph_value>::failure(lowered.error());
      }
      return wh::core::any(std::move(lowered).value());
    }
    if (edge.source_output == node_contract::stream && edge.target_input == node_contract::value) {
      // stream->value must be lowered by the async input stage first.
      return wh::core::result<graph_value>::failure(wh::core::errc::not_supported);
    }
    return wh::core::result<graph_value>::failure(wh::core::errc::contract_violation);
  }
  }
  return wh::core::result<graph_value>::failure(wh::core::errc::internal_error);
}

inline auto graph::plan_value_output_consumers(
    const std::uint32_t source_node_id,
    const std::optional<std::vector<std::uint32_t>> &selection) const
    -> value_output_consumers {
  const auto selected = [&selection](const std::uint32_t target_id) noexcept {
    if (!selection.has_value()) {
      return true;
    }
    return std::binary_search(selection->begin(), selection->end(), target_id);
  };

  value_output_consumers consumers{};
  const auto outgoing =
      core().compiled_execution_index_.index.outgoing_data(source_node_id);
  consumers.value_edges.reserve(outgoing.size());
  consumers.stream_edges.reserve(outgoing.size());
  for (const auto edge_id : outgoing) {
    const auto &edge = core().compiled_execution_index_.index.indexed_edges[edge_id];
    if (edge.no_data || edge.source_output != node_contract::value ||
        !selected(edge.to)) {
      continue;
    }
    if (edge.target_input == node_contract::value) {
      consumers.value_edges.push_back(edge_id);
      continue;
    }
    consumers.stream_edges.push_back(edge_id);
  }
  consumers.final_output =
      source_node_id == core().compiled_execution_index_.index.end_id;
  return consumers;
}

inline auto graph::lower_value_output_reader(const indexed_edge &edge,
                                             graph_value value,
                                             wh::core::run_context &context) const
    -> wh::core::result<graph_stream_reader> {
  switch (edge.lowering_kind) {
  case edge_lowering_kind::value_to_stream:
    return make_single_value_stream_reader(std::move(value));
  case edge_lowering_kind::custom:
    if (!edge.adapter.custom.to_stream.has_value()) {
      return wh::core::result<graph_stream_reader>::failure(
          wh::core::errc::contract_violation);
    }
    return (*edge.adapter.custom.to_stream)(std::move(value), edge.limits,
                                            context);
  case edge_lowering_kind::none:
  case edge_lowering_kind::stream_to_value:
    return wh::core::result<graph_stream_reader>::failure(
        wh::core::errc::contract_violation);
  }
  return wh::core::result<graph_stream_reader>::failure(
      wh::core::errc::internal_error);
}

inline auto graph::store_node_output(const std::uint32_t node_id, io_storage &storage,
                                     graph_value value) const -> wh::core::result<void> {
  const auto *node = core().compiled_execution_index_.index.nodes_by_id[node_id];
  if (node == nullptr) {
    return wh::core::result<void>::failure(wh::core::errc::not_found);
  }
  if (node->meta.output_contract == node_contract::value) {
    if (node_id == core().compiled_execution_index_.index.start_id) {
      auto boundary = detail::validate_value_boundary_payload(value);
      if (boundary.has_error()) {
        return wh::core::result<void>::failure(boundary.error());
      }
    } else {
      auto valid = detail::validate_value_contract_payload(value);
      if (valid.has_error()) {
        return wh::core::result<void>::failure(valid.error());
      }
    }
    auto owned = detail::materialize_value_payload(std::move(value));
    if (owned.has_error()) {
      return wh::core::result<void>::failure(owned.error());
    }
    storage.mark_value_output(node_id, std::move(owned).value());
    return {};
  }
  auto *reader = wh::core::any_cast<graph_stream_reader>(&value);
  if (reader == nullptr) {
    return wh::core::result<void>::failure(wh::core::errc::contract_violation);
  }
  if (node_id != core().compiled_execution_index_.index.end_id) {
    return wh::core::result<void>::failure(wh::core::errc::not_supported);
  }
  storage.mark_final_output_reader(node_id, std::move(*reader));
  return {};
}

inline auto graph::commit_value_output(
    const std::uint32_t source_node_id, io_storage &storage,
    graph_value value,
    const std::optional<std::vector<std::uint32_t>> &selection,
    wh::core::run_context &context) const -> wh::core::result<void> {
  const auto *node = core().compiled_execution_index_.index.nodes_by_id[source_node_id];
  if (node == nullptr || node->meta.output_contract != node_contract::value) {
    return wh::core::result<void>::failure(wh::core::errc::contract_violation);
  }

  value_output_consumers consumers =
      plan_value_output_consumers(source_node_id, selection);
  std::size_t remaining = consumers.owner_count();
  std::vector<std::pair<std::uint32_t, graph_value>> staged_values{};
  std::vector<std::pair<std::uint32_t, graph_stream_reader>> staged_readers{};
  std::optional<graph_value> final_output{};
  staged_values.reserve(consumers.value_edges.size());
  staged_readers.reserve(consumers.stream_edges.size());

  auto next_value = [&]() -> wh::core::result<graph_value> {
    if (remaining == 0U) {
      return wh::core::result<graph_value>::failure(
          wh::core::errc::internal_error);
    }
    if (remaining == 1U) {
      --remaining;
      return std::move(value);
    }
    --remaining;
    auto cloned = detail::materialize_value_payload(value);
    if (cloned.has_error()) {
      const auto code = cloned.error() == wh::core::errc::not_supported
                            ? wh::core::errc::contract_violation
                            : cloned.error();
      return wh::core::result<graph_value>::failure(code);
    }
    return std::move(cloned).value();
  };

  for (const auto edge_id : consumers.value_edges) {
    auto staged = next_value();
    if (staged.has_error()) {
      return wh::core::result<void>::failure(staged.error());
    }
    staged_values.emplace_back(edge_id, std::move(staged).value());
  }

  for (const auto edge_id : consumers.stream_edges) {
    auto staged = next_value();
    if (staged.has_error()) {
      return wh::core::result<void>::failure(staged.error());
    }
    const auto &edge = core().compiled_execution_index_.index.indexed_edges[edge_id];
    auto reader =
        lower_value_output_reader(edge, std::move(staged).value(), context);
    if (reader.has_error()) {
      return wh::core::result<void>::failure(reader.error());
    }
    staged_readers.emplace_back(edge_id, std::move(reader).value());
  }

  if (consumers.final_output) {
    auto staged = next_value();
    if (staged.has_error()) {
      return wh::core::result<void>::failure(staged.error());
    }
    final_output.emplace(std::move(staged).value());
  }

  const auto outgoing =
      core().compiled_execution_index_.index.outgoing_data(source_node_id);
  for (const auto edge_id : outgoing) {
    storage.edge_value_valid.clear(edge_id);
    storage.edge_reader_valid.clear(edge_id);
  }

  for (auto &[edge_id, staged] : staged_values) {
    storage.edge_values[edge_id] = std::move(staged);
    storage.edge_value_valid.set(edge_id);
  }

  for (auto &[edge_id, staged] : staged_readers) {
    storage.edge_readers[edge_id] = std::move(staged);
    storage.edge_reader_valid.set(edge_id);
  }

  if (final_output.has_value()) {
    storage.final_output_reader.reset();
    storage.node_values[source_node_id] = std::move(*final_output);
  }

  storage.output_valid.set(source_node_id);
  return {};
}

inline auto graph::view_node_output(const std::uint32_t node_id, io_storage &storage) const
    -> wh::core::result<graph_value> {
  const auto *node = core().compiled_execution_index_.index.nodes_by_id[node_id];
  if (node == nullptr || !storage.output_valid.test(node_id)) {
    return wh::core::result<graph_value>::failure(wh::core::errc::not_found);
  }
  if (node->meta.output_contract == node_contract::value) {
    if (node_id != core().compiled_execution_index_.index.end_id) {
      return wh::core::result<graph_value>::failure(
          wh::core::errc::contract_violation);
    }
    auto &value = storage.node_values[node_id];
    if (value.copyable()) {
      return graph_value{value};
    }
    return value.as_ref();
  }
  if (node_id != core().compiled_execution_index_.index.end_id ||
      !storage.final_output_reader.has_value()) {
    return wh::core::result<graph_value>::failure(wh::core::errc::not_found);
  }
  auto forked = detail::fork_graph_reader(*storage.final_output_reader);
  if (forked.has_error()) {
    return wh::core::result<graph_value>::failure(forked.error());
  }
  return graph_value{std::move(forked).value()};
}

inline auto graph::take_node_output(const std::uint32_t node_id, io_storage &storage) const
    -> wh::core::result<graph_value> {
  const auto *node = core().compiled_execution_index_.index.nodes_by_id[node_id];
  if (node == nullptr || !storage.output_valid.test(node_id)) {
    return wh::core::result<graph_value>::failure(wh::core::errc::not_found);
  }
  if (node->meta.output_contract == node_contract::value) {
    if (node_id != core().compiled_execution_index_.index.end_id) {
      return wh::core::result<graph_value>::failure(
          wh::core::errc::contract_violation);
    }
    return std::move(storage.node_values[node_id]);
  }
  if (node_id != core().compiled_execution_index_.index.end_id ||
      !storage.final_output_reader.has_value()) {
    return wh::core::result<graph_value>::failure(wh::core::errc::not_found);
  }
  auto reader = std::move(*storage.final_output_reader);
  storage.final_output_reader.reset();
  return graph_value{std::move(reader)};
}

inline auto graph::commit_stream_output(
    const std::uint32_t source_node_id, io_storage &storage, graph_stream_reader reader,
    const std::optional<std::vector<std::uint32_t>> &selection) const
    -> wh::core::result<void> {
  const auto selected = [&selection](const std::uint32_t target_id) noexcept {
    if (!selection.has_value()) {
      return true;
    }
    return std::binary_search(selection->begin(), selection->end(), target_id);
  };

  std::vector<std::uint32_t> consumer_edges{};
  const auto outgoing = core().compiled_execution_index_.index.outgoing_data(source_node_id);
  consumer_edges.reserve(outgoing.size());
  for (const auto edge_id : outgoing) {
    storage.edge_value_valid.clear(edge_id);
    storage.edge_reader_valid.clear(edge_id);

    const auto &edge = core().compiled_execution_index_.index.indexed_edges[edge_id];
    if (edge.no_data || edge.source_output != node_contract::stream || !selected(edge.to)) {
      continue;
    }
    consumer_edges.push_back(edge_id);
  }

  const bool needs_boundary_output =
      source_node_id == core().compiled_execution_index_.index.end_id;
  const auto consumer_count =
      consumer_edges.size() + static_cast<std::size_t>(needs_boundary_output);

  storage.mark_stream_output(source_node_id);
  if (consumer_count == 0U) {
    return {};
  }

  if (consumer_count == 1U) {
    if (needs_boundary_output) {
      storage.final_output_reader.emplace(std::move(reader));
      return {};
    }
    storage.edge_readers[consumer_edges.front()] = std::move(reader);
    storage.edge_reader_valid.set(consumer_edges.front());
    return {};
  }

  auto copied = detail::copy_graph_readers(std::move(reader), consumer_count);
  if (copied.has_error()) {
    return wh::core::result<void>::failure(copied.error());
  }

  auto readers = std::move(copied).value();
  if (readers.size() != consumer_count) {
    return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
  }

  std::size_t reader_index = 0U;
  for (const auto edge_id : consumer_edges) {
    storage.edge_readers[edge_id] = std::move(readers[reader_index++]);
    storage.edge_reader_valid.set(edge_id);
  }
  if (needs_boundary_output) {
    storage.final_output_reader.emplace(std::move(readers[reader_index]));
  }
  return {};
}

inline auto graph::resolve_edge_value(const std::uint32_t edge_id, io_storage &storage,
                                      wh::core::run_context &context) const
    -> wh::core::result<graph_value *> {
  if (edge_id >= core().compiled_execution_index_.index.indexed_edges.size() ||
      edge_id >= storage.edge_values.size()) {
    return wh::core::result<graph_value *>::failure(wh::core::errc::not_found);
  }
  if (storage.edge_value_valid.test(edge_id)) {
    return std::addressof(storage.edge_values[edge_id]);
  }

  const auto &edge = core().compiled_execution_index_.index.indexed_edges[edge_id];
  if (!storage.output_valid.test(edge.from)) {
    return wh::core::result<graph_value *>::failure(wh::core::errc::not_found);
  }
  if (edge.target_input != node_contract::value || needs_reader_lowering(edge)) {
    return wh::core::result<graph_value *>::failure(wh::core::errc::not_supported);
  }

  if (edge.source_output == node_contract::value) {
    if (edge.lowering_kind == edge_lowering_kind::none &&
        storage.edge_value_valid.test(edge_id)) {
      return std::addressof(storage.edge_values[edge_id]);
    }
    return wh::core::result<graph_value *>::failure(wh::core::errc::contract_violation);
  }

  if (edge.lowering_kind != edge_lowering_kind::custom) {
    return wh::core::result<graph_value *>::failure(wh::core::errc::contract_violation);
  }
  auto reader = take_edge_reader(edge_id, storage);
  if (reader.has_error()) {
    return wh::core::result<graph_value *>::failure(reader.error());
  }
  auto stream_input = wh::core::any(std::move(reader).value());
  auto adapted = adapt_edge_output(edge, stream_input, context);
  if (adapted.has_error()) {
    return wh::core::result<graph_value *>::failure(adapted.error());
  }
  storage.edge_values[edge_id] = std::move(adapted).value();
  storage.edge_value_valid.set(edge_id);
  return std::addressof(storage.edge_values[edge_id]);
}

inline auto graph::resolve_edge_reader(const std::uint32_t edge_id, io_storage &storage) const
    -> wh::core::result<graph_stream_reader *> {
  if (edge_id >= core().compiled_execution_index_.index.indexed_edges.size() ||
      edge_id >= storage.edge_readers.size()) {
    return wh::core::result<graph_stream_reader *>::failure(wh::core::errc::not_found);
  }
  if (storage.edge_reader_valid.test(edge_id)) {
    return std::addressof(storage.edge_readers[edge_id]);
  }

  const auto &edge = core().compiled_execution_index_.index.indexed_edges[edge_id];
  if (!storage.output_valid.test(edge.from)) {
    return wh::core::result<graph_stream_reader *>::failure(wh::core::errc::not_found);
  }
  if (edge.target_input != node_contract::stream) {
    return wh::core::result<graph_stream_reader *>::failure(wh::core::errc::type_mismatch);
  }

  if (edge.source_output == node_contract::stream) {
    if (edge.lowering_kind != edge_lowering_kind::none) {
      return wh::core::result<graph_stream_reader *>::failure(wh::core::errc::contract_violation);
    }
    if (!storage.edge_reader_valid.test(edge_id)) {
      return wh::core::result<graph_stream_reader *>::failure(wh::core::errc::not_found);
    }
    return std::addressof(storage.edge_readers[edge_id]);
  }

  if (storage.edge_reader_valid.test(edge_id)) {
    return std::addressof(storage.edge_readers[edge_id]);
  }
  return wh::core::result<graph_stream_reader *>::failure(
      wh::core::errc::contract_violation);
}

inline auto graph::take_edge_reader(const std::uint32_t edge_id, io_storage &storage) const
    -> wh::core::result<graph_stream_reader> {
  if (edge_id >= core().compiled_execution_index_.index.indexed_edges.size()) {
    return wh::core::result<graph_stream_reader>::failure(wh::core::errc::not_found);
  }

  const auto &edge = core().compiled_execution_index_.index.indexed_edges[edge_id];
  if (edge.source_output == node_contract::stream) {
    auto resolved = resolve_edge_reader(edge_id, storage);
    if (resolved.has_error()) {
      return wh::core::result<graph_stream_reader>::failure(resolved.error());
    }
    auto reader = std::move(*resolved.value());
    storage.edge_reader_valid.clear(edge_id);
    return reader;
  }

  auto resolved = resolve_edge_reader(edge_id, storage);
  if (resolved.has_error()) {
    return wh::core::result<graph_stream_reader>::failure(resolved.error());
  }
  auto reader = std::move(*resolved.value());
  storage.edge_reader_valid.clear(edge_id);
  return reader;
}

inline auto graph::merged_reader(const std::uint32_t node_id, io_storage &storage) const
    -> wh::core::result<graph_stream_reader *> {
  if (!needs_reader_merge(node_id)) {
    return wh::core::result<graph_stream_reader *>::failure(wh::core::errc::invalid_argument);
  }
  if (!storage.merged_reader_valid.test(node_id)) {
    std::vector<std::string> sources{};
    const auto &reader_edges = core().compiled_execution_index_.plan.inputs[node_id].reader_edges;
    sources.reserve(reader_edges.size());
    for (const auto edge_id : reader_edges) {
      sources.push_back(
          core()
              .compiled_execution_index_.index
              .id_to_key[core().compiled_execution_index_.index.indexed_edges[edge_id].from]);
    }
    storage.merged_readers[node_id] = detail::make_graph_merge_reader(std::move(sources));
    storage.merged_reader_valid.set(node_id);
  }
  return std::addressof(storage.merged_readers[node_id]);
}

inline auto graph::update_merged_reader(const std::uint32_t node_id, io_storage &storage,
                                        input_lane_span lanes) const
    -> wh::core::result<void> {
  if (!needs_reader_merge(node_id)) {
    return {};
  }

  auto merged = merged_reader(node_id, storage);
  if (merged.has_error()) {
    return wh::core::result<void>::failure(merged.error());
  }
  auto *shell =
      merged.value()
          ->template target_if<wh::schema::stream::merge_stream_reader<
              graph_stream_reader,
              wh::schema::stream::merge_topology_mode::dynamic_injection>>();
  if (shell == nullptr) {
    return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
  }

  for (const auto &lane : lanes) {
    auto &lane_state = storage.merged_reader_lane_states[lane.edge_id];
    if (lane_state != reader_lane_state::unseen) {
      continue;
    }
    if (lane.status == edge_status::waiting ||
        (lane.status == edge_status::active && !lane.output_ready)) {
      continue;
    }

    const auto &source_key = core().compiled_execution_index_.index.id_to_key[lane.source_id];
    if (lane.status == edge_status::active) {
      auto attached_reader = take_edge_reader(lane.edge_id, storage);
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

inline auto graph::refresh_merged_reader(const std::uint32_t node_id, io_storage &storage,
                                         const std::vector<dag_node_phase> &dag_node_phases,
                                         const std::vector<branch_state> &branch_states) const
    -> wh::core::result<void> {
  const auto lanes =
      collect_input_lanes(node_id, dag_node_phases, branch_states, storage.output_valid);
  return update_merged_reader(node_id, storage, lanes);
}

inline auto graph::build_reader_input(const compiled_node &node, const std::uint32_t node_id,
                                      io_storage &storage,
                                      input_lane_span lanes) const
    -> wh::core::result<resolved_input> {
  if (needs_reader_merge(node_id)) {
    auto synced = update_merged_reader(node_id, storage, lanes);
    if (synced.has_error()) {
      return wh::core::result<resolved_input>::failure(synced.error());
    }
    return resolved_input::borrow_reader(storage.merged_readers[node_id]);
  }

  for (std::size_t offset = lanes.size(); offset > 0U; --offset) {
    const auto &lane = lanes[offset - 1U];
    if (lane.status != edge_status::active || !lane.output_ready) {
      continue;
    }

    auto reader = take_edge_reader(lane.edge_id, storage);
    if (reader.has_error()) {
      return wh::core::result<resolved_input>::failure(reader.error());
    }
    return resolved_input::own_reader(std::move(reader).value());
  }

  return build_missing_input(node);
}

inline auto graph::build_value_input(const compiled_node &node, io_storage &storage,
                                     input_lane_span lanes,
                                     wh::core::run_context &context) const
    -> wh::core::result<resolved_input> {
  value_batch batch{
      .form = lanes.size() > 1U ? detail::input_runtime::value_input_form::fan_in
                                : detail::input_runtime::value_input_form::direct,
  };
  batch.fan_in.reserve(lanes.size());

  for (const auto &lane : lanes) {
    if (lane.status != edge_status::active) {
      continue;
    }
    if (!lane.output_ready) {
      if (core().options_.fan_in_policy != graph_fan_in_policy::allow_partial) {
        return wh::core::result<resolved_input>::failure(wh::core::errc::not_found);
      }
      continue;
    }

    auto edge_output = resolve_edge_value(lane.edge_id, storage, context);
    if (edge_output.has_error()) {
      return wh::core::result<resolved_input>::failure(edge_output.error());
    }
    value_input entry{};
    entry.source_id = lane.source_id;
    entry.edge_id = lane.edge_id;
    entry.borrowed = edge_output.value();
    if (storage.edge_value_valid.test(lane.edge_id)) {
      entry.owned.emplace(std::move(storage.edge_values[lane.edge_id]));
      storage.edge_value_valid.clear(lane.edge_id);
    }
    auto appended = detail::input_runtime::append_value_input(batch, std::move(entry));
    if (appended.has_error()) {
      return wh::core::result<resolved_input>::failure(appended.error());
    }
  }

  auto resolved = finish_value_input(node, std::move(batch));
  if (resolved.has_error() && resolved.error() == wh::core::errc::not_found) {
    return build_missing_input(node);
  }
  return resolved;
}

inline auto graph::finish_value_input(const compiled_node &node, value_batch batch) const
    -> wh::core::result<resolved_input> {
  const auto build_value_input_map =
      [this](std::vector<value_input> &entries) -> wh::core::result<resolved_input> {
    auto fan_in_input =
        detail::input_runtime::build_value_input_map(entries, [this](const value_input &entry) {
          return core().compiled_execution_index_.index.id_to_key[entry.source_id];
        });
    if (fan_in_input.has_error()) {
      return wh::core::result<resolved_input>::failure(fan_in_input.error());
    }
    return resolved_input::own_value(wh::core::any(std::move(fan_in_input).value()));
  };

  switch (batch.form) {
  case detail::input_runtime::value_input_form::direct:
    if (!batch.single.has_value()) {
      return wh::core::result<resolved_input>::failure(wh::core::errc::not_found);
    }
    if (batch.single->owned.has_value()) {
      return resolved_input::own_value(std::move(*batch.single->owned));
    }
    return resolved_input::borrow_value(*batch.single->value());
  case detail::input_runtime::value_input_form::fan_in: {
    const auto gate = compiled_value_input_gate(node);
    if (batch.fan_in.empty()) {
      return wh::core::result<resolved_input>::failure(wh::core::errc::not_found);
    }
    if (gate.kind == input_gate_kind::value_exact &&
        gate.value.key() != wh::core::any_type_key_v<graph_value_map>) {
      return wh::core::result<resolved_input>::failure(wh::core::errc::contract_violation);
    }
    return build_value_input_map(batch.fan_in);
  }
  }
  return wh::core::result<resolved_input>::failure(wh::core::errc::internal_error);
}

inline auto graph::refresh_source_readers(const std::uint32_t source_node_id,
                                          io_storage &storage,
                                          const std::vector<dag_node_phase> &dag_node_phases,
                                          const std::vector<branch_state> &branch_states) const
    -> wh::core::result<void> {
  for (const auto edge_id :
       core().compiled_execution_index_.plan.outputs[source_node_id].reader_edges) {
    const auto target_id = core().compiled_execution_index_.index.indexed_edges[edge_id].to;
    if (!needs_reader_merge(target_id) || !storage.merged_reader_valid.test(target_id)) {
      continue;
    }
    auto refreshed =
        refresh_merged_reader(target_id, storage, dag_node_phases, branch_states);
    if (refreshed.has_error()) {
      return refreshed;
    }
  }
  return {};
}

inline auto graph::collect_input_lanes(const std::uint32_t node_id,
                                       const std::vector<dag_node_phase> &dag_node_phases,
                                       const std::vector<branch_state> &branch_states,
                                       const dynamic_bitset &output_valid) const
    -> input_lane_vector {
  input_lane_vector lanes{};
  const auto incoming = core().compiled_execution_index_.index.incoming_data(node_id);
  lanes.reserve(incoming.size());
  for (const auto edge_id : incoming) {
    const auto &edge = core().compiled_execution_index_.index.indexed_edges[edge_id];
    const auto status = resolve_edge_status_indexed(edge, dag_node_phases, branch_states);
    lanes.push_back(input_lane{
        .edge_id = edge_id,
        .source_id = edge.from,
        .status = status,
        .output_ready = status == edge_status::active && output_valid.test(edge.from),
    });
  }
  return lanes;
}

inline auto graph::build_missing_input(const compiled_node &node) const
    -> wh::core::result<resolved_input> {
  if (node.meta.options.allow_no_data) {
    auto missing = make_missing_pending_input_default(node.meta.input_contract);
    if (missing.has_error()) {
      return wh::core::result<resolved_input>::failure(missing.error());
    }
    return own_input(std::move(missing).value(), node.meta.input_contract);
  }
  return wh::core::result<resolved_input>::failure(wh::core::errc::not_found);
}

inline auto graph::borrow_input(graph_value &value, const node_contract contract)
    -> wh::core::result<resolved_input> {
  if (contract == node_contract::stream) {
    auto *reader = wh::core::any_cast<graph_stream_reader>(&value);
    if (reader == nullptr) {
      return wh::core::result<resolved_input>::failure(wh::core::errc::type_mismatch);
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
      return wh::core::result<resolved_input>::failure(wh::core::errc::type_mismatch);
    }
    return resolved_input::own_reader(std::move(*reader));
  }
  return resolved_input::own_value(std::move(value));
}

inline auto
graph::build_node_input(const std::uint32_t node_id, io_storage &storage,
                        const std::vector<dag_node_phase> &dag_node_phases,
                        const std::vector<branch_state> &branch_states,
                        [[maybe_unused]] graph_value &scratch, wh::core::run_context &context,
                        [[maybe_unused]] const detail::runtime_state::invoke_config &config) const
    -> wh::core::result<resolved_input> {
  const auto *node = core().compiled_execution_index_.index.nodes_by_id[node_id];
  if (node == nullptr) {
    return wh::core::result<resolved_input>::failure(wh::core::errc::not_found);
  }

  const auto lanes =
      collect_input_lanes(node_id, dag_node_phases, branch_states, storage.output_valid);
  if (node->meta.input_contract == node_contract::stream) {
    return build_reader_input(*node, node_id, storage, lanes);
  }
  return build_value_input(*node, storage, lanes, context);
}

inline auto graph::build_node_input_sender(
    const std::uint32_t node_id, io_storage &storage, const std::vector<dag_node_phase> &dag_node_phases,
    const std::vector<branch_state> &branch_states, wh::core::run_context &context,
    attempt_slot *slot, const detail::runtime_state::invoke_config &config,
    const wh::core::detail::any_resume_scheduler_t &graph_scheduler) const -> graph_sender {
  if (node_id >= core().compiled_execution_index_.index.nodes_by_id.size()) {
    return detail::failure_graph_sender(wh::core::errc::not_found);
  }
  const auto *node = core().compiled_execution_index_.index.nodes_by_id[node_id];
  if (node == nullptr) {
    return detail::failure_graph_sender(wh::core::errc::not_found);
  }

  const auto finalize_input =
      [this, node,
       &context](wh::core::result<resolved_input> resolved) -> wh::core::result<graph_value> {
    if (resolved.has_error() && resolved.error() == wh::core::errc::not_found &&
        context.resume_info.has_value()) {
      auto fallback = resolve_missing_pending_input(node->meta.input_contract);
      if (fallback.has_error()) {
        return wh::core::result<graph_value>::failure(fallback.error());
      }
      auto lifted = own_input(std::move(fallback).value(), node->meta.input_contract);
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

  if (node->meta.input_contract == node_contract::stream) {
    graph_value scratch{};
    auto resolved =
        build_node_input(node_id, storage, dag_node_phases, branch_states, scratch, context, config);
    auto finalized = finalize_input(std::move(resolved));
    if (finalized.has_error()) {
      return detail::failure_graph_sender(finalized.error());
    }
    return detail::ready_graph_sender(std::move(finalized));
  }

  const auto lanes =
      collect_input_lanes(node_id, dag_node_phases, branch_states, storage.output_valid);
  const bool preserve_stream_pre =
      slot != nullptr && detail::state_runtime::has_stream_phase(
                             slot->state_handlers, detail::state_runtime::state_phase::pre);
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
      const auto &edge = core().compiled_execution_index_.index.indexed_edges[edge_id];
      if (needs_reader_lowering(edge)) {
        auto reader = take_edge_reader(edge_id, storage);
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
  }

  value_batch base_batch{
      .form = lanes.size() > 1U ? detail::input_runtime::value_input_form::fan_in
                                : detail::input_runtime::value_input_form::direct,
  };
  base_batch.fan_in.reserve(lanes.size());

  std::vector<std::uint32_t> async_edges{};
  async_edges.reserve(lanes.size());
  bool blocked = false;
  for (const auto &lane : lanes) {
    if (lane.status != edge_status::active) {
      continue;
    }
    if (!lane.output_ready) {
      if (core().options_.fan_in_policy != graph_fan_in_policy::allow_partial) {
        blocked = true;
        break;
      }
      continue;
    }

    const auto &edge = core().compiled_execution_index_.index.indexed_edges[lane.edge_id];
    if (!storage.edge_value_valid.test(lane.edge_id) && needs_reader_lowering(edge)) {
      async_edges.push_back(lane.edge_id);
      continue;
    }

    auto edge_output = resolve_edge_value(lane.edge_id, storage, context);
    if (edge_output.has_error()) {
      return detail::failure_graph_sender(edge_output.error());
    }
    value_input entry{};
    entry.source_id = lane.source_id;
    entry.edge_id = lane.edge_id;
    entry.borrowed = edge_output.value();
    if (storage.edge_value_valid.test(lane.edge_id)) {
      entry.owned.emplace(std::move(storage.edge_values[lane.edge_id]));
      storage.edge_value_valid.clear(lane.edge_id);
    }
    auto appended = detail::input_runtime::append_value_input(base_batch, std::move(entry));
    if (appended.has_error()) {
      return detail::failure_graph_sender(appended.error());
    }
  }

  if (async_edges.empty()) {
    auto resolved = blocked ? wh::core::result<resolved_input>::failure(wh::core::errc::not_found)
                            : finish_value_input(*node, std::move(base_batch));
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
    bool blocked{false};
    std::vector<std::uint32_t> edge_ids{};
  };

  input_stage input_state{
      .owner = this,
      .node = node,
      .batch = std::move(base_batch),
      .blocked = blocked,
      .edge_ids = std::move(async_edges),
  };

  std::vector<graph_sender> senders{};
  senders.reserve(input_state.edge_ids.size());
  for (const auto edge_id : input_state.edge_ids) {
    const auto &edge = core().compiled_execution_index_.index.indexed_edges[edge_id];
    auto reader = take_edge_reader(edge_id, storage);
    if (reader.has_error()) {
      return detail::failure_graph_sender(reader.error());
    }
    auto lowering = make_reader_lowering(edge);
    if (lowering.has_error()) {
      return detail::failure_graph_sender(lowering.error());
    }
    senders.push_back(lower_reader(std::move(reader).value(), std::move(lowering).value(), context,
                                   graph_scheduler));
  }

  return detail::bridge_graph_sender(detail::make_child_batch_sender(
      std::move(senders), std::move(input_state),
      [](input_stage &stage_state, const std::size_t index,
         wh::core::result<graph_value> current) -> wh::core::result<void> {
        if (index >= stage_state.edge_ids.size()) {
          return wh::core::result<void>::failure(wh::core::errc::internal_error);
        }
        if (current.has_error()) {
          return wh::core::result<void>::failure(current.error());
        }

        value_input entry{};
        entry.source_id = stage_state.owner->core()
                              .compiled_execution_index_.index.indexed_edges[stage_state.edge_ids[index]]
                              .from;
        entry.edge_id = stage_state.edge_ids[index];
        entry.owned.emplace(std::move(current).value());
        return detail::input_runtime::append_value_input(stage_state.batch, std::move(entry));
      },
      [finalize_input](input_stage &&stage_state) -> wh::core::result<graph_value> {
        auto resolved = stage_state.blocked
                            ? wh::core::result<resolved_input>::failure(wh::core::errc::not_found)
                            : stage_state.owner->finish_value_input(*stage_state.node,
                                                                    std::move(stage_state.batch));
        auto finalized = finalize_input(std::move(resolved));
        if (finalized.has_error()) {
          return wh::core::result<graph_value>::failure(finalized.error());
        }
        return std::move(finalized).value();
      },
      graph_scheduler));
}

} // namespace wh::compose
