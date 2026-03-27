// Defines pending-input snapshot capture used by interrupt/checkpoint paths.
#pragma once

#include "wh/compose/graph/graph.hpp"

namespace wh::compose {

inline auto detail::invoke_runtime::run_state::capture_pending_inputs()
    -> graph_sender {
  struct pending_input {
    std::uint32_t node_id{0U};
  };

  const auto node_count = static_cast<std::uint32_t>(
      owner_->runtime_cache_.index.nodes_by_id.size());
  std::vector<pending_input> pending{};
  pending.reserve(node_count);

  std::vector<graph_sender> senders{};
  senders.reserve(node_count);

  for (std::uint32_t node_id = 0U; node_id < node_count; ++node_id) {
    if (node_id == owner_->runtime_cache_.index.start_id ||
        node_id == owner_->runtime_cache_.index.end_id ||
        node_states()[node_id] != node_state::pending) {
      continue;
    }

    if (rerun_state_.contains(node_id)) {
      continue;
    }

    pending.push_back(pending_input{.node_id = node_id});
    senders.push_back(owner_->build_node_input_sender(
        node_id, scratch_, node_states(), branch_states(), context_, nullptr,
        invoke_config_));
  }

  if (senders.empty()) {
    return detail::ready_graph_sender(
        wh::core::result<graph_value>{wh::core::any(std::monostate{})});
  }

  return detail::bridge_graph_sender(
      wh::core::detail::make_concurrent_sender_vector<
          wh::core::result<graph_value>>(std::move(senders), 1U) |
      stdexec::let_value([this, pending = std::move(pending)](
                             std::vector<wh::core::result<graph_value>> status)
                             mutable -> graph_sender {
        if (status.size() != pending.size()) {
          return detail::failure_graph_sender(wh::core::errc::internal_error);
        }

        for (std::size_t index = 0U; index < pending.size(); ++index) {
          auto &entry = pending[index];
          auto &current = status[index];
          if (current.has_value()) {
            rerun_state_.store(entry.node_id, std::move(current).value());
            continue;
          }

          if (current.error() != wh::core::errc::not_found) {
            return detail::failure_graph_sender(current.error());
          }

          const auto *node = owner_->runtime_cache_.index.nodes_by_id[entry.node_id];
          if (node == nullptr || !node->meta.options.allow_no_data) {
            continue;
          }

          auto missing =
              owner_->resolve_missing_rerun_input(node->meta.input_contract);
          if (missing.has_error()) {
            return detail::failure_graph_sender(missing.error());
          }
          rerun_state_.store(entry.node_id, std::move(missing).value());
        }

        return detail::ready_graph_sender(
            wh::core::result<graph_value>{wh::core::any(std::monostate{})});
      }));
}

} // namespace wh::compose
