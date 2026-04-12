// Defines Pregel pending-input snapshot capture used by interrupt/checkpoint paths.
#pragma once

#include "wh/compose/graph/graph.hpp"
#include "wh/compose/graph/detail/runtime/pregel_run_state.hpp"

namespace wh::compose {

inline auto
detail::invoke_runtime::pregel_run_state::capture_pregel_pending_inputs()
    -> graph_sender {
  struct pending_input {
    std::uint32_t node_id{0U};
  };

  std::vector<pending_input> pending{};
  pending.reserve(pregel_delivery_.current_frontier().size() +
                  pregel_delivery_.next_nodes.size());

  std::vector<graph_sender> senders{};
  senders.reserve(pregel_delivery_.current_frontier().size() +
                  pregel_delivery_.next_nodes.size());

  detail::dynamic_bitset seen{};
  const auto &index = compiled_graph_index();
  seen.reset(index.nodes_by_id.size(), false);
  const auto append_pending = [this, &index, &pending, &senders, &seen](
                                  const std::uint32_t node_id,
                                  const pregel_node_inputs &inputs) -> void {
    if (node_id == index.start_id || node_id == index.end_id ||
        node_states()[node_id] != node_state::pending ||
        rerun_state_.contains(node_id) || !seen.set_if_unset(node_id)) {
      return;
    }

    pending.push_back(pending_input{.node_id = node_id});
    senders.push_back(owner_->build_pregel_node_input_sender(
        node_id, inputs, io_storage_, context_, nullptr, invoke_state().config,
        *invoke_state().graph_scheduler));
  };

  for (const auto node_id : pregel_delivery_.current_frontier()) {
    append_pending(node_id, pregel_delivery_.current[node_id]);
  }
  for (const auto node_id : pregel_delivery_.next_nodes) {
    append_pending(node_id, pregel_delivery_.next[node_id]);
  }

  if (senders.empty()) {
    return detail::ready_graph_unit_sender();
  }

  struct pending_capture_stage {
    std::vector<pending_input> pending{};
  };

  return detail::bridge_graph_sender(detail::make_child_batch_sender(
      std::move(senders),
      pending_capture_stage{.pending = std::move(pending)},
      [this, &index](pending_capture_stage &stage,
                     const std::size_t pending_index,
             wh::core::result<graph_value> current) -> wh::core::result<void> {
        if (pending_index >= stage.pending.size()) {
          return wh::core::result<void>::failure(wh::core::errc::internal_error);
        }

        auto &entry = stage.pending[pending_index];
        if (current.has_value()) {
          rerun_state_.store(entry.node_id, std::move(current).value());
          return {};
        }

        if (current.error() != wh::core::errc::not_found) {
          return wh::core::result<void>::failure(current.error());
        }

        const auto *node = index.nodes_by_id[entry.node_id];
        if (node == nullptr || !node->meta.options.allow_no_data) {
          return {};
        }

        auto missing =
            owner_->resolve_missing_rerun_input(node->meta.input_contract);
        if (missing.has_error()) {
          return wh::core::result<void>::failure(missing.error());
        }
        rerun_state_.store(entry.node_id, std::move(missing).value());
        return {};
      },
      [](pending_capture_stage &&) -> wh::core::result<graph_value> {
        return make_graph_unit_value();
      },
      *invoke_state().graph_scheduler));
}

} // namespace wh::compose
