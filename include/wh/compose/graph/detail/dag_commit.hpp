// Defines DAG-specific node-output commit helpers.
#pragma once

#include "wh/compose/graph/graph.hpp"
#include "wh/compose/graph/detail/runtime/dag_run_state.hpp"
#include "wh/core/compiler.hpp"

namespace wh::compose {

template <typename enqueue_fn_t>
inline auto detail::invoke_runtime::dag_run_state::commit_dag_node_output(
    node_frame &&frame, graph_value node_output,
    enqueue_fn_t &&enqueue_fn) -> wh::core::result<void> {
  wh_invariant(frame.node != nullptr);
  auto &invoke = invoke_state();
  auto release_node_local_state = [&]() noexcept {
    frame.node_local_scope.release(node_local_process_states_);
  };

  auto stored_output =
      owner_->store_node_output(frame.node_id, io_storage_, std::move(node_output));
  if (stored_output.has_error()) {
    owner_->publish_node_run_error(invoke.outputs, frame.node_scope.path,
                                   frame.node_id, stored_output.error(),
                                   "node output contract mismatch");
    state_table_.update(frame.node_id, graph_node_lifecycle_state::failed, 1U,
                        stored_output.error());
    append_transition(frame.node_id, graph_state_transition_event{
                                       .kind = graph_state_transition_kind::node_fail,
                                       .cause = frame.cause,
                                       .lifecycle =
                                           graph_node_lifecycle_state::failed,
                                   });
    persist_checkpoint_best_effort();
    release_node_local_state();
    return wh::core::result<void>::failure(stored_output.error());
  }
  auto output_view = owner_->view_node_output(frame.node_id, io_storage_);
  if (output_view.has_error()) {
    persist_checkpoint_best_effort();
    release_node_local_state();
    return wh::core::result<void>::failure(output_view.error());
  }
  const auto branch_contract = frame.node->meta.output_contract;
  auto resolved_branch =
      branch_contract == node_contract::value
          ? owner_->evaluate_value_branch_indexed(
                frame.node_id, output_view.value(), context_,
                invoke.bound_call_scope)
          : owner_->evaluate_stream_branch_indexed(frame.node_id, io_storage_,
                                                   context_,
                                                   invoke.bound_call_scope);
  if (resolved_branch.has_error()) {
    owner_->publish_node_run_error(invoke.outputs, frame.node_scope.path,
                                   frame.node_id, resolved_branch.error(),
                                   "branch selector failed");
    state_table_.update(frame.node_id, graph_node_lifecycle_state::failed, 1U,
                        resolved_branch.error());
    append_transition(frame.node_id, graph_state_transition_event{
                                       .kind = graph_state_transition_kind::node_fail,
                                       .cause = frame.cause,
                                       .lifecycle =
                                           graph_node_lifecycle_state::failed,
                                   });
    persist_checkpoint_best_effort();
    release_node_local_state();
    return wh::core::result<void>::failure(resolved_branch.error());
  }
  auto branch_committed = owner_->commit_branch_selection(
      frame.node_id, std::move(resolved_branch).value(), dag_schedule_,
      invoke.config);
  if (branch_committed.has_error()) {
    owner_->publish_node_run_error(invoke.outputs, frame.node_scope.path,
                                   frame.node_id, branch_committed.error(),
                                   "branch commit failed");
    state_table_.update(frame.node_id, graph_node_lifecycle_state::failed, 1U,
                        branch_committed.error());
    append_transition(frame.node_id, graph_state_transition_event{
                                       .kind = graph_state_transition_kind::node_fail,
                                       .cause = frame.cause,
                                       .lifecycle =
                                           graph_node_lifecycle_state::failed,
                                   });
    persist_checkpoint_best_effort();
    release_node_local_state();
    return wh::core::result<void>::failure(branch_committed.error());
  }
  node_states()[frame.node_id] = node_state::executed;
  auto refreshed_streams = owner_->refresh_source_readers(
      frame.node_id, io_storage_, node_states(), branch_states(), context_);
  if (refreshed_streams.has_error()) {
    owner_->publish_node_run_error(invoke.outputs, frame.node_scope.path,
                                   frame.node_id, refreshed_streams.error(),
                                   "merged stream refresh failed");
    state_table_.update(frame.node_id, graph_node_lifecycle_state::failed, 1U,
                        refreshed_streams.error());
    append_transition(frame.node_id, graph_state_transition_event{
                                       .kind = graph_state_transition_kind::node_fail,
                                       .cause = frame.cause,
                                       .lifecycle =
                                           graph_node_lifecycle_state::failed,
                                   });
    persist_checkpoint_best_effort();
    release_node_local_state();
    return wh::core::result<void>::failure(refreshed_streams.error());
  }
  state_table_.update(frame.node_id, graph_node_lifecycle_state::completed, 0U,
                      std::nullopt);
  append_transition(frame.node_id, graph_state_transition_event{
                                     .kind = graph_state_transition_kind::route_commit,
                                     .cause = frame.cause,
                                     .lifecycle =
                                         graph_node_lifecycle_state::completed,
                                 });
  append_transition(frame.node_id, graph_state_transition_event{
                                     .kind = graph_state_transition_kind::node_leave,
                                     .cause = frame.cause,
                                     .lifecycle =
                                         graph_node_lifecycle_state::completed,
                                 });
  auto post_output = owner_->view_node_output(frame.node_id, io_storage_);
  if (post_output.has_error()) {
    persist_checkpoint_best_effort();
    release_node_local_state();
    return wh::core::result<void>::failure(post_output.error());
  }
  auto post_interrupt = owner_->evaluate_interrupt_hook(
      context_, invoke.config.interrupt_post_hook, frame.cause.node_key,
      post_output.value());
  if (post_interrupt.has_error()) {
    persist_checkpoint_best_effort();
    release_node_local_state();
    return wh::core::result<void>::failure(post_interrupt.error());
  }
  if (post_interrupt.value().has_value()) {
    context_.interrupt_info =
        wh::compose::to_interrupt_context(std::move(post_interrupt).value().value());
    emit_debug(graph_debug_stream_event::decision_kind::interrupt_hit, frame.node_id,
               invoke.step_count);
    request_freeze(false);
    release_node_local_state();
    return wh::core::result<void>::failure(wh::core::errc::canceled);
  }

  if (!interrupt_state().freeze_requested) {
    enqueue_fn(frame.node_id);
  }
  release_node_local_state();
  return {};
}

} // namespace wh::compose
