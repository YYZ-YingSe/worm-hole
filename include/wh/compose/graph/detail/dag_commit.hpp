// Defines DAG-specific node-output commit helpers.
#pragma once

#include "wh/compose/graph/detail/runtime/dag_runtime.hpp"
#include "wh/compose/graph/graph.hpp"
#include "wh/core/compiler.hpp"

namespace wh::compose {

template <typename enqueue_fn_t>
inline auto detail::invoke_runtime::dag_runtime::commit_node_output(const attempt_id attempt,
                                                                    graph_value node_output,
                                                                    enqueue_fn_t &&enqueue_fn)
    -> wh::core::result<void> {
  auto &attempt_slot = session_.slot(attempt);
  wh_invariant(attempt_slot.node != nullptr);
  auto &session = session_;
  auto &invoke = session.invoke_state();

  const auto branch_contract = attempt_slot.node->meta.output_contract;
  std::optional<std::vector<std::uint32_t>> resolved_branch{};
  std::optional<wh::core::interrupt_signal> post_interrupt_signal{};

  if (branch_contract == node_contract::value) {
    auto branch_status = session.owner_->evaluate_value_branch_indexed(
        attempt_slot.node_id, node_output, session.context_, invoke.bound_call_scope);
    if (branch_status.has_error()) {
      session.owner_->publish_node_run_error(invoke.outputs, attempt_slot.node_scope.path,
                                             attempt_slot.node_id, branch_status.error(),
                                             "branch selector failed");
      session.state_table_.update(attempt_slot.node_id, graph_node_lifecycle_state::failed, 1U,
                                  branch_status.error());
      session.append_transition(attempt_slot.node_id,
                                graph_state_transition_event{
                                    .kind = graph_state_transition_kind::node_fail,
                                    .cause = attempt_slot.cause,
                                    .lifecycle = graph_node_lifecycle_state::failed,
                                });
      try_persist_checkpoint();
      session.release_attempt(attempt);
      return wh::core::result<void>::failure(branch_status.error());
    }
    resolved_branch = std::move(branch_status).value();

    auto post_interrupt = session.evaluate_post_interrupt(attempt_slot.cause, node_output);
    if (post_interrupt.has_error()) {
      try_persist_checkpoint();
      session.release_attempt(attempt);
      return wh::core::result<void>::failure(post_interrupt.error());
    }
    if (post_interrupt.value().has_value()) {
      post_interrupt_signal.emplace(std::move(post_interrupt).value().value());
    }

    auto committed_value = session.owner_->commit_value_output(
        attempt_slot.node_id, session.io_storage_, std::move(node_output), resolved_branch,
        session.context_);
    if (committed_value.has_error()) {
      session.owner_->publish_node_run_error(invoke.outputs, attempt_slot.node_scope.path,
                                             attempt_slot.node_id, committed_value.error(),
                                             "node output contract mismatch");
      session.state_table_.update(attempt_slot.node_id, graph_node_lifecycle_state::failed, 1U,
                                  committed_value.error());
      session.append_transition(attempt_slot.node_id,
                                graph_state_transition_event{
                                    .kind = graph_state_transition_kind::node_fail,
                                    .cause = attempt_slot.cause,
                                    .lifecycle = graph_node_lifecycle_state::failed,
                                });
      try_persist_checkpoint();
      session.release_attempt(attempt);
      return wh::core::result<void>::failure(committed_value.error());
    }
  } else {
    return wh::core::result<void>::failure(wh::core::errc::not_supported);
  }

  auto branch_committed = session.owner_->commit_branch_selection(
      attempt_slot.node_id, std::move(resolved_branch), dag_schedule_, invoke.config);
  if (branch_committed.has_error()) {
    session.owner_->publish_node_run_error(invoke.outputs, attempt_slot.node_scope.path,
                                           attempt_slot.node_id, branch_committed.error(),
                                           "branch commit failed");
    session.state_table_.update(attempt_slot.node_id, graph_node_lifecycle_state::failed, 1U,
                                branch_committed.error());
    session.append_transition(attempt_slot.node_id,
                              graph_state_transition_event{
                                  .kind = graph_state_transition_kind::node_fail,
                                  .cause = attempt_slot.cause,
                                  .lifecycle = graph_node_lifecycle_state::failed,
                              });
    try_persist_checkpoint();
    session.release_attempt(attempt);
    return wh::core::result<void>::failure(branch_committed.error());
  }

  dag_node_phases()[attempt_slot.node_id] = invoke_session::dag_node_phase::executed;
  auto refreshed_streams = session.owner_->refresh_source_readers(
      attempt_slot.node_id, session.io_storage_, dag_node_phases(), branch_states());
  if (refreshed_streams.has_error()) {
    session.owner_->publish_node_run_error(invoke.outputs, attempt_slot.node_scope.path,
                                           attempt_slot.node_id, refreshed_streams.error(),
                                           "merged stream refresh failed");
    session.state_table_.update(attempt_slot.node_id, graph_node_lifecycle_state::failed, 1U,
                                refreshed_streams.error());
    session.append_transition(attempt_slot.node_id,
                              graph_state_transition_event{
                                  .kind = graph_state_transition_kind::node_fail,
                                  .cause = attempt_slot.cause,
                                  .lifecycle = graph_node_lifecycle_state::failed,
                              });
    try_persist_checkpoint();
    session.release_attempt(attempt);
    return wh::core::result<void>::failure(refreshed_streams.error());
  }
  session.state_table_.update(attempt_slot.node_id, graph_node_lifecycle_state::completed, 0U,
                              std::nullopt);
  session.append_transition(attempt_slot.node_id,
                            graph_state_transition_event{
                                .kind = graph_state_transition_kind::route_commit,
                                .cause = attempt_slot.cause,
                                .lifecycle = graph_node_lifecycle_state::completed,
                            });
  session.append_transition(attempt_slot.node_id,
                            graph_state_transition_event{
                                .kind = graph_state_transition_kind::node_leave,
                                .cause = attempt_slot.cause,
                                .lifecycle = graph_node_lifecycle_state::completed,
                            });

  if (post_interrupt_signal.has_value()) {
    session.apply_post_interrupt_signal(attempt_slot.node_id, attempt_slot.cause,
                                        std::move(*post_interrupt_signal));
    session.release_attempt(attempt);
    return wh::core::result<void>::failure(wh::core::errc::canceled);
  }

  if (!session.interrupt_state().freeze_requested) {
    enqueue_fn(attempt_slot.node_id);
  }
  session.release_attempt(attempt);
  return {};
}

inline auto detail::invoke_runtime::dag_runtime::commit_routed_output(
    const attempt_id attempt, std::optional<std::vector<std::uint32_t>> selection)
    -> wh::core::result<void> {
  auto &session = session_;
  auto &invoke = session.invoke_state();
  auto &attempt_slot = session.slot(attempt);
  std::optional<wh::core::interrupt_signal> post_interrupt_signal{};

  if (!attempt_slot.route.has_value()) {
    return wh::core::result<void>::failure(wh::core::errc::not_found);
  }

  if (std::holds_alternative<route_start_entry_state>(*attempt_slot.route)) {
    auto *stream_output = invoke.pending_start_entry_output.has_value()
                              ? wh::core::any_cast<graph_stream_reader>(
                                    &*invoke.pending_start_entry_output)
                              : nullptr;
    if (stream_output == nullptr) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }

    auto committed_start =
        session.owner_->commit_stream_output(session.compiled_graph_index().start_id,
                                             session.io_storage_, std::move(*stream_output), selection);
    if (committed_start.has_error()) {
      session.release_attempt(attempt);
      return wh::core::result<void>::failure(committed_start.error());
    }

    invoke.start_entry_selection = selection;
    invoke.pending_start_entry_output.reset();
    session.release_attempt(attempt);

    const auto &index = session.compiled_graph_index();
    dag_node_phases_[index.start_id] = invoke_session::dag_node_phase::executed;
    auto start_branch_committed = session.owner_->commit_branch_selection(
        index.start_id, invoke.start_entry_selection, dag_schedule_, invoke.config);
    if (start_branch_committed.has_error()) {
      session.state_table_.update(index.start_id, graph_node_lifecycle_state::failed, 1U,
                                  start_branch_committed.error());
      session.append_transition(index.start_id,
                                graph_state_transition_event{
                                    .kind = graph_state_transition_kind::node_fail,
                                    .cause =
                                        graph_state_cause{
                                            .run_id = invoke.run_id,
                                            .step = 0U,
                                            .node_key = std::string{graph_start_node_key},
                                        },
                                    .lifecycle = graph_node_lifecycle_state::failed,
                                });
      return wh::core::result<void>::failure(start_branch_committed.error());
    }

    auto start_streams = session.owner_->refresh_source_readers(index.start_id, session.io_storage_,
                                                                dag_node_phases_, branch_states());
    if (start_streams.has_error()) {
      return wh::core::result<void>::failure(start_streams.error());
    }

    enqueue_dependents(index.start_id);
    for (const auto node_id : index.allow_no_control_ids) {
      if (frontier_.enqueue_current(node_id)) {
        session.emit_debug(graph_debug_stream_event::decision_kind::enqueue, node_id,
                           session.invoke_state().step_count);
      }
    }
    invoke.start_entry_selection.reset();
    return {};
  }

  if (auto *route = std::get_if<route_node_output_state>(&*attempt_slot.route); route != nullptr) {
    const auto committed_node_id = attempt_slot.node_id;
    auto *stream_output = wh::core::any_cast<graph_stream_reader>(&route->node_output);
    if (stream_output == nullptr) {
      return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
    }

    auto post_interrupt = session.evaluate_post_interrupt_stream(attempt_slot.cause, *stream_output);
    if (post_interrupt.has_error()) {
      try_persist_checkpoint();
      session.release_attempt(attempt);
      return wh::core::result<void>::failure(post_interrupt.error());
    }
    if (post_interrupt.value().has_value()) {
      post_interrupt_signal.emplace(std::move(post_interrupt).value().value());
    }

    auto committed_stream = session.owner_->commit_stream_output(attempt_slot.node_id, session.io_storage_,
                                                                 std::move(*stream_output), selection);
    if (committed_stream.has_error()) {
      session.owner_->publish_node_run_error(invoke.outputs, attempt_slot.node_scope.path,
                                             attempt_slot.node_id, committed_stream.error(),
                                             "node output contract mismatch");
      session.state_table_.update(attempt_slot.node_id, graph_node_lifecycle_state::failed, 1U,
                                  committed_stream.error());
      session.append_transition(attempt_slot.node_id,
                                graph_state_transition_event{
                                    .kind = graph_state_transition_kind::node_fail,
                                    .cause = attempt_slot.cause,
                                    .lifecycle = graph_node_lifecycle_state::failed,
                                });
      session.release_attempt(attempt);
      return wh::core::result<void>::failure(committed_stream.error());
    }

    auto branch_committed = session.owner_->commit_branch_selection(attempt_slot.node_id,
                                                                    std::move(selection), dag_schedule_,
                                                                    invoke.config);
    if (branch_committed.has_error()) {
      session.owner_->publish_node_run_error(invoke.outputs, attempt_slot.node_scope.path,
                                             attempt_slot.node_id, branch_committed.error(),
                                             "branch commit failed");
      session.state_table_.update(attempt_slot.node_id, graph_node_lifecycle_state::failed, 1U,
                                  branch_committed.error());
      session.append_transition(attempt_slot.node_id,
                                graph_state_transition_event{
                                    .kind = graph_state_transition_kind::node_fail,
                                    .cause = attempt_slot.cause,
                                    .lifecycle = graph_node_lifecycle_state::failed,
                                });
      session.release_attempt(attempt);
      return wh::core::result<void>::failure(branch_committed.error());
    }

    dag_node_phases_[attempt_slot.node_id] = invoke_session::dag_node_phase::executed;
    auto refreshed_streams = session.owner_->refresh_source_readers(
        attempt_slot.node_id, session.io_storage_, dag_node_phases_, branch_states());
    if (refreshed_streams.has_error()) {
      session.owner_->publish_node_run_error(invoke.outputs, attempt_slot.node_scope.path,
                                             attempt_slot.node_id, refreshed_streams.error(),
                                             "merged stream refresh failed");
      session.state_table_.update(attempt_slot.node_id, graph_node_lifecycle_state::failed, 1U,
                                  refreshed_streams.error());
      session.append_transition(attempt_slot.node_id,
                                graph_state_transition_event{
                                    .kind = graph_state_transition_kind::node_fail,
                                    .cause = attempt_slot.cause,
                                    .lifecycle = graph_node_lifecycle_state::failed,
                                });
      session.release_attempt(attempt);
      return wh::core::result<void>::failure(refreshed_streams.error());
    }

    session.state_table_.update(attempt_slot.node_id, graph_node_lifecycle_state::completed, 0U,
                                std::nullopt);
    session.append_transition(attempt_slot.node_id,
                              graph_state_transition_event{
                                  .kind = graph_state_transition_kind::route_commit,
                                  .cause = attempt_slot.cause,
                                  .lifecycle = graph_node_lifecycle_state::completed,
                              });
    session.append_transition(attempt_slot.node_id,
                              graph_state_transition_event{
                                  .kind = graph_state_transition_kind::node_leave,
                                  .cause = attempt_slot.cause,
                                  .lifecycle = graph_node_lifecycle_state::completed,
                              });

    if (post_interrupt_signal.has_value()) {
      session.apply_post_interrupt_signal(committed_node_id, attempt_slot.cause,
                                          std::move(*post_interrupt_signal));
      session.release_attempt(attempt);
      return wh::core::result<void>::failure(wh::core::errc::canceled);
    }
    session.release_attempt(attempt);
    if (!session.interrupt_state().freeze_requested) {
      enqueue_dependents(committed_node_id);
    }
    return {};
  }

  return wh::core::result<void>::failure(wh::core::errc::not_supported);
}

} // namespace wh::compose
