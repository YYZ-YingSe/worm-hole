// Defines Pregel-specific graph-run entry initialization.
#pragma once

#include "wh/compose/graph/graph.hpp"
#include "wh/compose/graph/detail/runtime/checkpoint/pregel.hpp"
#include "wh/compose/graph/detail/runtime/pregel_runtime.hpp"

namespace wh::compose {

inline auto detail::invoke_runtime::pregel_runtime::restore_entry(
    detail::checkpoint_runtime::prepared_restore &prepared)
    -> wh::core::result<void> {
  auto &session = session_;
  if (!prepared.checkpoint.runtime.pregel.has_value() ||
      prepared.checkpoint.runtime.dag.has_value()) {
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
      std::move(prepared.checkpoint.runtime.pregel->pending_inputs),
      session.pending_inputs_, session.compiled_graph_index().start_id,
      session.node_count());
  if (restored_pending.has_error()) {
    return restored_pending;
  }

  auto restored_runtime = detail::checkpoint_runtime::restore_pregel_runtime(
      *prepared.checkpoint.runtime.pregel, session.io_storage_,
      prepared.checkpoint.runtime.lifecycle, pregel_delivery_,
      session.node_count(),
      superstep_active_);
  if (restored_runtime.has_error()) {
    return restored_runtime;
  }

  if (session.restore_skip_pre_handlers_) {
    auto marked = detail::checkpoint_runtime::mark_restored_pregel_pending_nodes(
        session.pending_inputs_, *prepared.checkpoint.runtime.pregel,
        session.node_count());
    if (marked.has_error()) {
      return marked;
    }
  }

  session.restore_plan_.reset();
  session.entry_input_.reset();
  return {};
}

inline auto
detail::invoke_runtime::pregel_runtime::start_entry(graph_value input)
    -> wh::core::result<void> {
  auto &session = session_;
  auto start_initialized =
      session.initialize_start_entry(std::move(input));
  if (start_initialized.has_error()) {
    return wh::core::result<void>::failure(start_initialized.error());
  }

  const auto &index = session.compiled_graph_index();
  session.owner_->seed_pregel_successors(index.start_id,
                                         session.invoke_state().start_entry_selection,
                                         pregel_delivery_);
  for (const auto node_id : index.allow_no_control_ids) {
    pregel_delivery_.stage_current_node(node_id);
  }
  superstep_active_ = false;
  session.invoke_state().start_entry_selection.reset();
  return {};
}

inline auto detail::invoke_runtime::pregel_runtime::initialize_entry()
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
      fail_restore(*session.restore_plan_, restored.error(),
                   "restore_pregel_entry");
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
