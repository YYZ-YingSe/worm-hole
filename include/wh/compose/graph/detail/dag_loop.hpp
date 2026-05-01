#pragma once

#include "wh/compose/graph/detail/runtime/dag_runtime.hpp"
#include "wh/compose/graph/detail/graph_class.hpp"
#include "wh/core/compiler.hpp"

namespace wh::compose {

inline auto detail::invoke_runtime::dag_runtime::take_ready_action() -> ready_action {
  auto &session = session_;
  auto &invoke = session.invoke_state();
  auto next_node = frontier().dequeue();
  if (!next_node.has_value()) {
    return ready_action::no_ready();
  }

  const auto node_id = *next_node;
  session.emit_debug(graph_debug_stream_event::decision_kind::dequeue, node_id, invoke.step_count);
  wh_invariant(node_id < dag_node_phases().size());
  if (dag_node_phases()[node_id] != invoke_session::dag_node_phase::pending) {
    return ready_action::continue_scan();
  }

  const auto &index = session.compiled_graph_index();
  const auto &node_key = index.id_to_key[node_id];
  if (!session.owner_->is_node_designated(node_id, invoke.bound_call_scope)) {
    dag_node_phases()[node_id] = invoke_session::dag_node_phase::skipped;
    session.state_table_.update(node_id, graph_node_lifecycle_state::skipped, 0U, std::nullopt);
    session.append_transition(node_id, graph_state_transition_event{
                                           .kind = graph_state_transition_kind::node_skip,
                                           .cause =
                                               graph_state_cause{
                                                   .run_id = invoke.run_id,
                                                   .step = invoke.step_count,
                                                   .node_key = node_key,
                                               },
                                           .lifecycle = graph_node_lifecycle_state::skipped,
                                       });
    session.emit_debug(graph_debug_stream_event::decision_kind::skipped, node_id,
                       invoke.step_count);
    auto refreshed_streams = session.owner_->refresh_source_readers(
        node_id, session.io_storage_, dag_node_phases(), branch_states());
    if (refreshed_streams.has_error()) {
      return ready_action::terminal_error(refreshed_streams.error());
    }
    enqueue_dependents(node_id);
    return ready_action::continue_scan();
  }

  const auto readiness = session.owner_->classify_node_readiness_indexed(
      node_id, dag_node_phases(), branch_states(), session.output_valid());
  if (readiness == invoke_session::ready_state::waiting) {
    return ready_action::continue_scan();
  }
  if (readiness == invoke_session::ready_state::skipped) {
    dag_node_phases()[node_id] = invoke_session::dag_node_phase::skipped;
    session.state_table_.update(node_id, graph_node_lifecycle_state::skipped, 0U, std::nullopt);
    session.append_transition(node_id, graph_state_transition_event{
                                           .kind = graph_state_transition_kind::node_skip,
                                           .cause =
                                               graph_state_cause{
                                                   .run_id = invoke.run_id,
                                                   .step = invoke.step_count,
                                                   .node_key = node_key,
                                               },
                                           .lifecycle = graph_node_lifecycle_state::skipped,
                                       });
    session.emit_debug(graph_debug_stream_event::decision_kind::skipped, node_id,
                       invoke.step_count);
    auto refreshed_streams = session.owner_->refresh_source_readers(
        node_id, session.io_storage_, dag_node_phases(), branch_states());
    if (refreshed_streams.has_error()) {
      return ready_action::terminal_error(refreshed_streams.error());
    }
    enqueue_dependents(node_id);
    return ready_action::continue_scan();
  }

  ++invoke.step_count;
  if (invoke.step_count > invoke.step_budget) {
    auto completed_node_keys = session.state_table_.collect_completed_keys();
    invoke.outputs.completed_node_keys = completed_node_keys;
    invoke.outputs.step_limit_error = graph_step_limit_error_detail{
        .step = invoke.step_count,
        .budget = invoke.step_budget,
        .node = node_key,
        .completed_node_keys = std::move(completed_node_keys),
    };
    session.owner_->publish_graph_run_error(invoke.outputs, session.runtime_node_path(node_id),
                                            node_key, compose_error_phase::schedule,
                                            wh::core::errc::timeout, "step budget exceeded");
    try_persist_checkpoint();
    return ready_action::terminal_error(wh::core::errc::timeout);
  }

  const auto *node = index.nodes_by_id[node_id];
  if (node == nullptr) {
    session.state_table_.update(node_id, graph_node_lifecycle_state::failed, 1U,
                                wh::core::errc::not_found);
    session.append_transition(node_id, graph_state_transition_event{
                                           .kind = graph_state_transition_kind::node_fail,
                                           .cause =
                                               graph_state_cause{
                                                   .run_id = invoke.run_id,
                                                   .step = invoke.step_count,
                                                   .node_key = node_key,
                                               },
                                           .lifecycle = graph_node_lifecycle_state::failed,
                                       });
    try_persist_checkpoint();
    return ready_action::terminal_error(wh::core::errc::not_found);
  }

  auto attempt = make_input_attempt(node_id, invoke.step_count);
  if (attempt.has_error()) {
    session.state_table_.update(node_id, graph_node_lifecycle_state::failed, 1U, attempt.error());
    session.append_transition(node_id, graph_state_transition_event{
                                           .kind = graph_state_transition_kind::node_fail,
                                           .cause =
                                               graph_state_cause{
                                                   .run_id = invoke.run_id,
                                                   .step = invoke.step_count,
                                                   .node_key = node_key,
                                               },
                                           .lifecycle = graph_node_lifecycle_state::failed,
                                       });
    try_persist_checkpoint();
    return ready_action::terminal_error(attempt.error());
  }
  mark_suspended(node_id);
  return ready_action::launch(attempt.value());
}

} // namespace wh::compose
