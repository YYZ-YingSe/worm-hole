#pragma once

#include "wh/compose/graph/detail/runtime/pregel_run_state.hpp"
#include "wh/compose/graph/graph.hpp"

namespace wh::compose {

inline auto detail::invoke_runtime::pregel_run_state::take_next_pregel_action(
    const std::uint32_t node_id, const std::size_t step) -> pregel_action {
  auto &invoke = invoke_state();
  emit_debug(graph_debug_stream_event::decision_kind::dequeue, node_id, step);

  const auto &index = compiled_graph_index();
  const auto &node_key = index.id_to_key[node_id];
  const auto cause = graph_state_cause{
      .run_id = invoke.run_id,
      .step = step,
      .node_key = node_key,
  };
  if (!owner_->is_node_designated(node_id, invoke.bound_call_scope)) {
    return pregel_action::skip(node_id, cause);
  }

  if (owner_->classify_pregel_node_readiness(
          node_id, pregel_delivery_.current[node_id]) ==
      pregel_ready_state::skipped) {
    return pregel_action::skip(node_id, cause);
  }

  const auto *node = index.nodes_by_id[node_id];
  if (node == nullptr) {
    state_table_.update(node_id, graph_node_lifecycle_state::failed, 1U,
                        wh::core::errc::not_found);
    append_transition(node_id,
                      graph_state_transition_event{
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
    append_transition(node_id,
                      graph_state_transition_event{
                          .kind = graph_state_transition_kind::node_fail,
                          .cause = cause,
                          .lifecycle = graph_node_lifecycle_state::failed,
                      });
    persist_checkpoint_best_effort();
    return pregel_action::terminal_error(node_id, cause, frame.error());
  }

  return pregel_action::launch(std::move(frame).value());
}

} // namespace wh::compose
