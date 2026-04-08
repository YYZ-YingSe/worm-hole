// Defines DAG-specific graph-run entry initialization.
#pragma once

#include "wh/compose/graph/graph.hpp"
#include "wh/compose/graph/detail/runtime/dag_run_state.hpp"

namespace wh::compose {

inline auto detail::invoke_runtime::dag_run_state::initialize_dag_entry()
    -> void {
  const auto &index = compiled_graph_index();
  auto start_branch_committed = owner_->commit_branch_selection(
      index.start_id, invoke_state().start_entry_selection, dag_schedule_,
      invoke_state().config);
  if (start_branch_committed.has_error()) {
    state_table_.update(index.start_id, graph_node_lifecycle_state::failed, 1U,
                        start_branch_committed.error());
    append_transition(index.start_id,
                      graph_state_transition_event{
                          .kind = graph_state_transition_kind::node_fail,
                          .cause =
                              graph_state_cause{
                                  .run_id = invoke_state().run_id,
                                  .step = 0U,
                                  .node_key = std::string{graph_start_node_key},
                              },
                          .lifecycle = graph_node_lifecycle_state::failed,
                      });
    persist_checkpoint_best_effort();
    init_error_ = start_branch_committed.error();
    return;
  }

  auto start_streams = owner_->refresh_source_readers(
      index.start_id, io_storage_, node_states(), branch_states(), context_);
  if (start_streams.has_error()) {
    init_error_ = start_streams.error();
    return;
  }

  enqueue_dependents(index.start_id);
  for (const auto node_id : index.allow_no_control_ids) {
    if (frontier().enqueue_current(node_id)) {
      emit_debug(graph_debug_stream_event::decision_kind::enqueue, node_id,
                 invoke_state().step_count);
    }
  }
  invoke_state().start_entry_selection.reset();
}

} // namespace wh::compose
