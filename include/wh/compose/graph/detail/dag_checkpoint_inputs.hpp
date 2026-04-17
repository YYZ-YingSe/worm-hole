// Defines DAG pending-input capture used by interrupt/checkpoint paths.
#pragma once

#include "wh/compose/graph/graph.hpp"
#include "wh/compose/graph/detail/runtime/dag_runtime.hpp"

namespace wh::compose {

inline auto detail::invoke_runtime::dag_runtime::capture_pending_inputs()
    -> graph_sender {
  auto &session = session_;
  auto &invoke = session.invoke_state();
  struct pending_input {
    std::uint32_t node_id{0U};
  };

  const auto &index = session.compiled_graph_index();
  const auto node_count =
      static_cast<std::uint32_t>(index.nodes_by_id.size());
  std::vector<pending_input> pending{};
  pending.reserve(node_count);

  std::vector<graph_sender> senders{};
  senders.reserve(node_count);

  for (std::uint32_t node_id = 0U; node_id < node_count; ++node_id) {
    if (node_id == index.start_id || node_id == index.end_id ||
        dag_node_phases()[node_id] != invoke_session::dag_node_phase::pending) {
      continue;
    }

    if (session.pending_inputs_.contains_input(node_id)) {
      continue;
    }

    pending.push_back(pending_input{.node_id = node_id});
    senders.push_back(session.owner_->build_node_input_sender(
        node_id, session.io_storage_, dag_node_phases(), branch_states(),
        session.context_, nullptr, invoke.config, *invoke.graph_scheduler));
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
      [this, &index](pending_capture_stage &stage, const std::size_t pending_index,
             wh::core::result<graph_value> current) -> wh::core::result<void> {
        if (pending_index >= stage.pending.size()) {
          return wh::core::result<void>::failure(wh::core::errc::internal_error);
        }

        auto &entry = stage.pending[pending_index];
        if (current.has_value()) {
          session_.pending_inputs_.store_input(entry.node_id,
                                               std::move(current).value());
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
            session_.owner_->resolve_missing_pending_input(node->meta.input_contract);
        if (missing.has_error()) {
          return wh::core::result<void>::failure(missing.error());
        }
        session_.pending_inputs_.store_input(entry.node_id,
                                             std::move(missing).value());
        return {};
      },
      [](pending_capture_stage &&) -> wh::core::result<graph_value> {
        return make_graph_unit_value();
      },
      *invoke.graph_scheduler));
}

} // namespace wh::compose
