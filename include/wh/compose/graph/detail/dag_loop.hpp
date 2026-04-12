#pragma once

#include "wh/compose/graph/detail/runtime/dag_run_state.hpp"
#include "wh/compose/graph/graph.hpp"
#include "wh/core/compiler.hpp"

namespace wh::compose {

inline auto detail::invoke_runtime::dag_run_state::take_next_ready_action()
    -> ready_action {
  auto &invoke = invoke_state();
  auto next_node = frontier().dequeue();
  if (!next_node.has_value()) {
    return ready_action::no_ready();
  }

  const auto node_id = *next_node;
  emit_debug(graph_debug_stream_event::decision_kind::dequeue, node_id,
             invoke.step_count);
  wh_invariant(node_id < node_states().size());
  if (node_states()[node_id] != node_state::pending) {
    return ready_action::continue_scan();
  }

  const auto &index = compiled_graph_index();
  const auto &node_key = index.id_to_key[node_id];
  if (!owner_->is_node_designated(node_id, invoke.bound_call_scope)) {
    node_states()[node_id] = node_state::skipped;
    state_table_.update(node_id, graph_node_lifecycle_state::skipped, 0U,
                        std::nullopt);
    append_transition(node_id,
                      graph_state_transition_event{
                          .kind = graph_state_transition_kind::node_skip,
                          .cause =
                              graph_state_cause{
                                  .run_id = invoke.run_id,
                                  .step = invoke.step_count,
                                  .node_key = node_key,
                              },
                          .lifecycle = graph_node_lifecycle_state::skipped,
                      });
    emit_debug(graph_debug_stream_event::decision_kind::skipped, node_id,
               invoke.step_count);
    auto refreshed_streams = owner_->refresh_source_readers(
        node_id, io_storage_, node_states(), branch_states(), context_);
    if (refreshed_streams.has_error()) {
      return ready_action::terminal_error(refreshed_streams.error());
    }
    enqueue_dependents(node_id);
    return ready_action::continue_scan();
  }

  const auto readiness = owner_->classify_node_readiness_indexed(
      node_id, node_states(), branch_states(), output_valid());
  if (readiness == ready_state::waiting) {
    return ready_action::continue_scan();
  }
  if (readiness == ready_state::skipped) {
    node_states()[node_id] = node_state::skipped;
    state_table_.update(node_id, graph_node_lifecycle_state::skipped, 0U,
                        std::nullopt);
    append_transition(node_id,
                      graph_state_transition_event{
                          .kind = graph_state_transition_kind::node_skip,
                          .cause =
                              graph_state_cause{
                                  .run_id = invoke.run_id,
                                  .step = invoke.step_count,
                                  .node_key = node_key,
                              },
                          .lifecycle = graph_node_lifecycle_state::skipped,
                      });
    emit_debug(graph_debug_stream_event::decision_kind::skipped, node_id,
               invoke.step_count);
    auto refreshed_streams = owner_->refresh_source_readers(
        node_id, io_storage_, node_states(), branch_states(), context_);
    if (refreshed_streams.has_error()) {
      return ready_action::terminal_error(refreshed_streams.error());
    }
    enqueue_dependents(node_id);
    return ready_action::continue_scan();
  }

  ++invoke.step_count;
  if (invoke.step_count > invoke.step_budget) {
    auto completed_nodes = owner_->collect_completed_nodes(node_states());
    invoke.outputs.last_completed_nodes = completed_nodes;
    invoke.outputs.step_limit_error = graph_step_limit_error_detail{
        .step = invoke.step_count,
        .budget = invoke.step_budget,
        .node = node_key,
        .completed_nodes = std::move(completed_nodes),
    };
    owner_->publish_graph_run_error(invoke.outputs, runtime_node_path(node_id),
                                    node_key, compose_error_phase::schedule,
                                    wh::core::errc::timeout,
                                    "step budget exceeded");
    persist_checkpoint_best_effort();
    return ready_action::terminal_error(wh::core::errc::timeout);
  }

  const auto *node = index.nodes_by_id[node_id];
  if (node == nullptr) {
    state_table_.update(node_id, graph_node_lifecycle_state::failed, 1U,
                        wh::core::errc::not_found);
    append_transition(node_id,
                      graph_state_transition_event{
                          .kind = graph_state_transition_kind::node_fail,
                          .cause =
                              graph_state_cause{
                                  .run_id = invoke.run_id,
                                  .step = invoke.step_count,
                                  .node_key = node_key,
                              },
                          .lifecycle = graph_node_lifecycle_state::failed,
                      });
    persist_checkpoint_best_effort();
    return ready_action::terminal_error(wh::core::errc::not_found);
  }

  auto frame = make_input_frame(node_id, invoke.step_count);
  if (frame.has_error()) {
    state_table_.update(node_id, graph_node_lifecycle_state::failed, 1U,
                        frame.error());
    append_transition(node_id,
                      graph_state_transition_event{
                          .kind = graph_state_transition_kind::node_fail,
                          .cause =
                              graph_state_cause{
                                  .run_id = invoke.run_id,
                                  .step = invoke.step_count,
                                  .node_key = node_key,
                              },
                          .lifecycle = graph_node_lifecycle_state::failed,
                      });
    persist_checkpoint_best_effort();
    return ready_action::terminal_error(frame.error());
  }
  return ready_action::launch(std::move(frame).value());
}

} // namespace wh::compose
