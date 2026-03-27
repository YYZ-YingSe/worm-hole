#pragma once

#include "wh/compose/graph/graph.hpp"

namespace wh::compose {

inline auto detail::invoke_runtime::run_state::take_next_pregel_action(
    const std::uint32_t node_id, const std::size_t step)
    -> pregel_action {
  emit_debug(graph_debug_stream_event::decision_kind::dequeue, node_id, step);
  queued().clear(node_id);
  if (node_states()[node_id] != node_state::pending) {
    return pregel_action::waiting(node_id);
  }

  const auto &node_key = owner_->runtime_cache_.index.id_to_key[node_id];
  const auto cause = graph_state_cause{
      .run_id = run_id_,
      .step = step,
      .node_key = node_key,
  };
  if (!owner_->is_node_designated(node_id, bound_call_scope_)) {
    return pregel_action::skip(node_id, cause);
  }

  const auto readiness = owner_->classify_node_readiness_indexed(
      node_id, node_states(), branch_states(), output_valid(), scratch_);
  if (readiness == ready_state::waiting) {
    return pregel_action::waiting(node_id);
  }
  if (readiness == ready_state::skipped) {
    return pregel_action::skip(node_id, cause);
  }

  const auto *node = owner_->runtime_cache_.index.nodes_by_id[node_id];
  if (node == nullptr) {
    state_table_.update(node_id, graph_node_lifecycle_state::failed, 1U,
                        wh::core::errc::not_found);
    append_transition(node_id, graph_state_transition_event{
        .kind = graph_state_transition_kind::node_fail,
        .cause = cause,
        .lifecycle = graph_node_lifecycle_state::failed,
    });
      persist_checkpoint_best_effort();
      return pregel_action::terminal_error(node_id, cause,
                                                    wh::core::errc::not_found);
  }

  auto frame = make_input_frame(node_id, step);
  if (frame.has_error()) {
    state_table_.update(node_id, graph_node_lifecycle_state::failed, 1U,
                        frame.error());
    append_transition(node_id, graph_state_transition_event{
        .kind = graph_state_transition_kind::node_fail,
        .cause = cause,
        .lifecycle = graph_node_lifecycle_state::failed,
    });
    persist_checkpoint_best_effort();
    return pregel_action::terminal_error(node_id, cause,
                                                  frame.error());
  }

  return pregel_action::launch(std::move(frame).value());
}

} // namespace wh::compose
