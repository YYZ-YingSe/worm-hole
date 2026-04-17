// Defines DAG-specific graph-run entry initialization.
#pragma once

#include "wh/compose/graph/graph.hpp"
#include "wh/compose/graph/detail/runtime/checkpoint/dag.hpp"
#include "wh/compose/graph/detail/runtime/dag_runtime.hpp"

namespace wh::compose {

inline auto detail::invoke_runtime::dag_runtime::restore_entry(
    detail::checkpoint_runtime::prepared_restore &prepared)
    -> wh::core::result<void> {
  auto &session = session_;
  if (!prepared.checkpoint.runtime.dag.has_value() ||
      prepared.checkpoint.runtime.pregel.has_value()) {
    return wh::core::result<void>::failure(
        wh::core::errc::contract_violation);
  }

  session.restore_skip_pre_handlers_ = prepared.restore_skip_pre_handlers;
  session.invoke_state().step_count = prepared.checkpoint.runtime.step_count;
  auto restored_states = detail::checkpoint_runtime::restore_node_states(
      prepared.checkpoint, session.state_table_);
  if (restored_states.has_error()) {
    return restored_states;
  }
  auto restored_pending = detail::checkpoint_runtime::restore_pending_inputs(
      std::move(prepared.checkpoint.runtime.dag->pending_inputs),
      session.pending_inputs_, session.compiled_graph_index().start_id,
      session.node_count());
  if (restored_pending.has_error()) {
    return restored_pending;
  }

  auto restored_phases = detail::checkpoint_runtime::restore_dag_node_phases(
      prepared.checkpoint.runtime.lifecycle, dag_node_phases_);
  if (restored_phases.has_error()) {
    return restored_phases;
  }

  std::vector<std::uint32_t> restored_suspended_nodes{};
  auto restored_runtime = detail::checkpoint_runtime::restore_dag_runtime(
      *prepared.checkpoint.runtime.dag, session.io_storage_,
      prepared.checkpoint.runtime.lifecycle, dag_node_phases_, dag_schedule_,
      frontier_, restored_suspended_nodes);
  if (restored_runtime.has_error()) {
    return restored_runtime;
  }

  if (session.restore_skip_pre_handlers_) {
    detail::checkpoint_runtime::mark_restored_dag_pending_nodes(
        session.pending_inputs_, dag_node_phases_);
  }

  restore_suspended_nodes(std::move(restored_suspended_nodes));
  session.restore_plan_.reset();
  session.entry_input_.reset();
  return {};
}

inline auto
detail::invoke_runtime::dag_runtime::start_entry(graph_value input)
    -> wh::core::result<void> {
  auto &session = session_;
  auto start_initialized =
      session.initialize_start_entry(std::move(input));
  if (start_initialized.has_error()) {
    return wh::core::result<void>::failure(start_initialized.error());
  }

  const auto &index = session.compiled_graph_index();
  dag_node_phases_[index.start_id] = invoke_session::dag_node_phase::executed;
  auto start_branch_committed = session.owner_->commit_branch_selection(
      index.start_id, session.invoke_state().start_entry_selection,
      dag_schedule_, session.invoke_state().config);
  if (start_branch_committed.has_error()) {
    session.state_table_.update(index.start_id,
                                graph_node_lifecycle_state::failed, 1U,
                                start_branch_committed.error());
    session.append_transition(index.start_id,
                              graph_state_transition_event{
                                  .kind = graph_state_transition_kind::node_fail,
                                  .cause =
                                      graph_state_cause{
                                          .run_id = session.invoke_state().run_id,
                                          .step = 0U,
                                          .node_key = std::string{graph_start_node_key},
                                      },
                                  .lifecycle = graph_node_lifecycle_state::failed,
                              });
    return wh::core::result<void>::failure(start_branch_committed.error());
  }

  auto start_streams = session.owner_->refresh_source_readers(
      index.start_id, session.io_storage_, dag_node_phases_, branch_states());
  if (start_streams.has_error()) {
    return wh::core::result<void>::failure(start_streams.error());
  }

  enqueue_dependents(index.start_id);
  for (const auto node_id : index.allow_no_control_ids) {
    if (frontier().enqueue_current(node_id)) {
      session.emit_debug(graph_debug_stream_event::decision_kind::enqueue,
                         node_id, session.invoke_state().step_count);
    }
  }
  session.invoke_state().start_entry_selection.reset();
  return {};
}

inline auto detail::invoke_runtime::dag_runtime::initialize_entry()
    -> void {
  auto &session = session_;
  const auto fail_restore =
      [this](const detail::checkpoint_runtime::prepared_restore &prepared,
             const wh::core::error_code code,
             const std::string_view operation) -> void {
    detail::checkpoint_runtime::set_error_detail(
        session_.invoke_state().outputs, code, prepared.checkpoint_id_hint,
        operation);
    session_.init_error_ = code;
  };

  if (session.restore_plan_.has_value()) {
    auto restored = restore_entry(*session.restore_plan_);
    if (restored.has_error()) {
      fail_restore(*session.restore_plan_, restored.error(), "restore_dag_entry");
    }
    return;
  }

  if (!session.entry_input_.has_value()) {
    session.init_error_ = wh::core::errc::internal_error;
    return;
  }

  auto started = start_entry(std::move(*session.entry_input_));
  session.entry_input_.reset();
  if (started.has_error()) {
    session.init_error_ = started.error();
  }
}

} // namespace wh::compose
