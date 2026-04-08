// Defines Pregel-specific node-output commit helpers.
#pragma once

#include "wh/compose/graph/graph.hpp"
#include "wh/compose/graph/detail/runtime/pregel_run_state.hpp"

namespace wh::compose {

template <typename enqueue_fn_t>
inline auto detail::invoke_runtime::pregel_run_state::commit_pregel_node_output(
    node_frame &&frame, graph_value node_output,
    enqueue_fn_t &&enqueue_fn) -> wh::core::result<void> {
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

  owner_->reset_pregel_source_caches(frame.node_id, io_storage_);
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

  node_states()[frame.node_id] = node_state::executed;
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

  auto post_interrupt = owner_->evaluate_interrupt_hook(
      context_, invoke.config.interrupt_post_hook, frame.cause.node_key,
      output_view.value());
  if (post_interrupt.has_error()) {
    persist_checkpoint_best_effort();
    release_node_local_state();
    return wh::core::result<void>::failure(post_interrupt.error());
  }
  if (post_interrupt.value().has_value()) {
    context_.interrupt_info = wh::compose::to_interrupt_context(
        std::move(post_interrupt).value().value());
    emit_debug(graph_debug_stream_event::decision_kind::interrupt_hit,
               frame.node_id, invoke.step_count);
    request_freeze(false);
    release_node_local_state();
    return wh::core::result<void>::failure(wh::core::errc::canceled);
  }

  if (!interrupt_state().freeze_requested) {
    owner_->stage_pregel_successors(frame.node_id,
                                    std::move(resolved_branch).value(),
                                    pregel_delivery_);
    enqueue_fn(frame.node_id);
  }
  release_node_local_state();
  return {};
}

inline auto detail::invoke_runtime::pregel_run_state::commit_pregel_skip_action(
    const pregel_action &action) -> wh::core::result<void> {
  node_states()[action.node_id] = node_state::skipped;
  state_table_.update(action.node_id, graph_node_lifecycle_state::skipped, 0U,
                      std::nullopt);
  append_transition(action.node_id, graph_state_transition_event{
                                     .kind = graph_state_transition_kind::node_skip,
                                     .cause = action.cause,
                                     .lifecycle =
                                         graph_node_lifecycle_state::skipped,
                                 });
  emit_debug(graph_debug_stream_event::decision_kind::skipped, action.node_id,
             action.cause.step);
  return {};
}

} // namespace wh::compose
