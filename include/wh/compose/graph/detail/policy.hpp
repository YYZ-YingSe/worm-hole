// Defines graph runtime policy, compiled-execution index, branch, and budget
// helpers.
#pragma once

#include "wh/compose/graph/graph.hpp"

namespace wh::compose {
inline auto graph::resolve_node_retry_budget(const std::uint32_t node_id) const
    -> std::size_t {
  const auto *node =
      core().compiled_execution_index_.index.nodes_by_id[node_id];
  if (node != nullptr && node->meta.options.retry_budget_override.has_value()) {
    return *node->meta.options.retry_budget_override;
  }
  return core().options_.retry_budget;
}

inline auto
graph::resolve_node_timeout_budget(const std::uint32_t node_id) const
    -> std::optional<std::chrono::milliseconds> {
  const auto *node =
      core().compiled_execution_index_.index.nodes_by_id[node_id];
  if (node != nullptr && node->meta.options.timeout_override.has_value()) {
    return node->meta.options.timeout_override;
  }
  return core().options_.node_timeout;
}

inline auto graph::resolve_node_parallel_gate(const std::uint32_t node_id) const
    -> std::size_t {
  const auto *node =
      core().compiled_execution_index_.index.nodes_by_id[node_id];
  if (node != nullptr && node->meta.options.max_parallel_override.has_value()) {
    return *node->meta.options.max_parallel_override;
  }
  return core().options_.max_parallel_per_node;
}

inline auto graph::resolve_branch_merge(
    const detail::runtime_state::invoke_config &config) noexcept
    -> graph_branch_merge {
  return config.branch_merge;
}

inline auto graph::merge_branch_selected_nodes(
    const std::vector<std::uint32_t> &existing_sorted,
    std::vector<std::uint32_t> incoming_sorted,
    const graph_branch_merge strategy)
    -> wh::core::result<std::vector<std::uint32_t>> {
  std::sort(incoming_sorted.begin(), incoming_sorted.end());
  incoming_sorted.erase(
      std::unique(incoming_sorted.begin(), incoming_sorted.end()),
      incoming_sorted.end());
  switch (strategy) {
  case graph_branch_merge::overwrite:
    return incoming_sorted;
  case graph_branch_merge::keep_existing:
    return existing_sorted;
  case graph_branch_merge::fail_on_conflict:
    if (existing_sorted == incoming_sorted) {
      return existing_sorted;
    }
    return wh::core::result<std::vector<std::uint32_t>>::failure(
        wh::core::errc::contract_violation);
  case graph_branch_merge::set_union:
    break;
  }

  std::vector<std::uint32_t> merged{};
  merged.reserve(existing_sorted.size() + incoming_sorted.size());
  std::set_union(existing_sorted.begin(), existing_sorted.end(),
                 incoming_sorted.begin(), incoming_sorted.end(),
                 std::back_inserter(merged));
  return merged;
}

inline auto graph::commit_branch_selection(
    const std::uint32_t node_id,
    std::optional<std::vector<std::uint32_t>> selection,
    dag_schedule &dag_schedule,
    const detail::runtime_state::invoke_config &config) const
    -> wh::core::result<void> {
  if (!selection.has_value()) {
    return {};
  }
  const auto strategy = resolve_branch_merge(config);

  auto &state = dag_schedule.branch_states[node_id];
  if (!state.decided) {
    dag_schedule.mark_branch_decided(node_id, std::move(selection).value());
    return {};
  }
  auto merged = merge_branch_selected_nodes(
      state.selected_end_nodes_sorted, std::move(selection).value(), strategy);
  if (merged.has_error()) {
    return wh::core::result<void>::failure(merged.error());
  }
  if (std::ranges::find(dag_schedule.decided_branch_nodes, node_id) ==
      dag_schedule.decided_branch_nodes.end()) {
    dag_schedule.decided_branch_nodes.push_back(node_id);
  }
  state.selected_end_nodes_sorted = std::move(merged).value();
  return {};
}

inline auto graph::evaluate_value_branch_indexed(
    const std::uint32_t source_node_id, const graph_value &source_output,
    wh::core::run_context &context, const graph_call_scope &call_options) const
    -> wh::core::result<std::optional<std::vector<std::uint32_t>>> {
  const auto *branch =
      core().compiled_execution_index_.index.value_branch_for_source(
          source_node_id);
  if (branch == nullptr) {
    return std::optional<std::vector<std::uint32_t>>{};
  }

  std::vector<std::uint32_t> selected{};
  if (!branch->selector_ids) {
    selected = branch->end_nodes_sorted;
    return std::optional<std::vector<std::uint32_t>>{std::move(selected)};
  }

  auto routed_ids = branch->selector_ids(source_output, context, call_options);
  if (routed_ids.has_error()) {
    return wh::core::result<std::optional<std::vector<std::uint32_t>>>::failure(
        routed_ids.error());
  }
  selected = std::move(routed_ids).value();
  for (const auto node_id : selected) {
    if (!branch->contains(node_id)) {
      return wh::core::result<std::optional<std::vector<std::uint32_t>>>::
          failure(wh::core::errc::contract_violation);
    }
  }
  std::sort(selected.begin(), selected.end());
  selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
  return std::optional<std::vector<std::uint32_t>>{std::move(selected)};
}

inline auto graph::evaluate_stream_branch_indexed(
    const std::uint32_t source_node_id, io_storage &io_storage,
    wh::core::run_context &context, const graph_call_scope &call_options) const
    -> wh::core::result<std::optional<std::vector<std::uint32_t>>> {
  const auto *branch =
      core().compiled_execution_index_.index.stream_branch_for_source(
          source_node_id);
  if (branch == nullptr) {
    return std::optional<std::vector<std::uint32_t>>{};
  }

  std::vector<std::uint32_t> selected{};
  if (!branch->selector_ids) {
    selected = branch->end_nodes_sorted;
    return std::optional<std::vector<std::uint32_t>>{std::move(selected)};
  }

  if (!io_storage.output_valid.test(source_node_id)) {
    return wh::core::result<std::optional<std::vector<std::uint32_t>>>::failure(
        wh::core::errc::not_found);
  }

  auto copied = detail::copy_graph_readers(
      std::move(io_storage.node_readers[source_node_id]), 2U);
  if (copied.has_error()) {
    return wh::core::result<std::optional<std::vector<std::uint32_t>>>::failure(
        copied.error());
  }

  auto readers = std::move(copied).value();
  io_storage.node_readers[source_node_id] = std::move(readers[0]);

  auto routed_ids =
      branch->selector_ids(std::move(readers[1]), context, call_options);
  if (routed_ids.has_error()) {
    return wh::core::result<std::optional<std::vector<std::uint32_t>>>::failure(
        routed_ids.error());
  }

  selected = std::move(routed_ids).value();
  for (const auto node_id : selected) {
    if (!branch->contains(node_id)) {
      return wh::core::result<std::optional<std::vector<std::uint32_t>>>::
          failure(wh::core::errc::contract_violation);
    }
  }
  std::sort(selected.begin(), selected.end());
  selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
  return std::optional<std::vector<std::uint32_t>>{std::move(selected)};
}

inline auto
graph::resolve_step_budget(const detail::runtime_state::invoke_config &config,
                           const graph_call_scope &call_options) const
    -> wh::core::result<std::size_t> {
  if (core().options_.mode != graph_runtime_mode::pregel) {
    if (call_options.pregel_max_steps().has_value()) {
      return wh::core::result<std::size_t>::failure(
          wh::core::errc::contract_violation);
    }
    return core().options_.max_steps;
  }
  if (call_options.pregel_max_steps().has_value()) {
    if (*call_options.pregel_max_steps() == 0U) {
      return wh::core::result<std::size_t>::failure(
          wh::core::errc::invalid_argument);
    }
    return *call_options.pregel_max_steps();
  }
  if (config.pregel_max_steps_override.has_value() &&
      *config.pregel_max_steps_override > 0U) {
    return *config.pregel_max_steps_override;
  }
  if (core().options_.max_steps == 0U) {
    return wh::core::result<std::size_t>::failure(
        wh::core::errc::invalid_argument);
  }
  return core().options_.max_steps;
}

} // namespace wh::compose
