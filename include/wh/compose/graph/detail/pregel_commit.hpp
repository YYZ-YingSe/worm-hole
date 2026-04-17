// Defines Pregel-specific node-output commit helpers.
#pragma once

#include "wh/compose/graph/graph.hpp"
#include "wh/compose/graph/detail/runtime/pregel_runtime.hpp"

namespace wh::compose {

template <typename enqueue_fn_t>
inline auto detail::invoke_runtime::pregel_runtime::commit_node_output(
    node_frame &&frame, graph_value node_output,
    enqueue_fn_t &&enqueue_fn) -> wh::core::result<void> {
  auto &session = session_;
  auto &invoke = session.invoke_state();
  auto release_node_local_state = [&]() noexcept {
    frame.node_local_scope.release(session.node_local_process_states_);
  };

  session.owner_->reset_pregel_source_caches(frame.node_id, session.io_storage_);
  const auto branch_contract = frame.node->meta.output_contract;
  std::optional<std::vector<std::uint32_t>> resolved_branch{};
  std::optional<wh::core::error_code> post_interrupt_error{};
  std::optional<wh::core::interrupt_signal> post_interrupt_signal{};

  if (branch_contract == node_contract::value) {
    auto branch_status = session.owner_->evaluate_value_branch_indexed(
        frame.node_id, node_output, session.context_, invoke.bound_call_scope);
    if (branch_status.has_error()) {
      session.owner_->publish_node_run_error(invoke.outputs, frame.node_scope.path,
                                             frame.node_id, branch_status.error(),
                                             "branch selector failed");
      session.state_table_.update(frame.node_id,
                                  graph_node_lifecycle_state::failed, 1U,
                                  branch_status.error());
      session.append_transition(frame.node_id, graph_state_transition_event{
                                         .kind = graph_state_transition_kind::node_fail,
                                         .cause = frame.cause,
                                         .lifecycle =
                                             graph_node_lifecycle_state::failed,
                                     });
      try_persist_checkpoint();
      release_node_local_state();
      return wh::core::result<void>::failure(branch_status.error());
    }
    resolved_branch = std::move(branch_status).value();

    auto post_interrupt = session.owner_->evaluate_interrupt_hook(
        session.context_, invoke.config.interrupt_post_hook, frame.cause.node_key,
        node_output);
    if (post_interrupt.has_error()) {
      try_persist_checkpoint();
      release_node_local_state();
      return wh::core::result<void>::failure(post_interrupt.error());
    }
    if (post_interrupt.value().has_value()) {
      post_interrupt_signal.emplace(std::move(post_interrupt).value().value());
    }

    auto committed_value = session.owner_->commit_value_output(
        frame.node_id, session.io_storage_, std::move(node_output),
        resolved_branch, session.context_);
    if (committed_value.has_error()) {
      session.owner_->publish_node_run_error(invoke.outputs, frame.node_scope.path,
                                             frame.node_id, committed_value.error(),
                                             "node output contract mismatch");
      session.state_table_.update(frame.node_id,
                                  graph_node_lifecycle_state::failed, 1U,
                                  committed_value.error());
      session.append_transition(frame.node_id, graph_state_transition_event{
                                         .kind = graph_state_transition_kind::node_fail,
                                         .cause = frame.cause,
                                         .lifecycle =
                                             graph_node_lifecycle_state::failed,
                                     });
      try_persist_checkpoint();
      release_node_local_state();
      return wh::core::result<void>::failure(committed_value.error());
    }
  } else {
    auto *stream_output = wh::core::any_cast<graph_stream_reader>(&node_output);
    if (stream_output == nullptr) {
      session.owner_->publish_node_run_error(
          invoke.outputs, frame.node_scope.path, frame.node_id,
          wh::core::errc::contract_violation, "node output contract mismatch");
      session.state_table_.update(frame.node_id,
                                  graph_node_lifecycle_state::failed, 1U,
                                  wh::core::errc::contract_violation);
      session.append_transition(frame.node_id, graph_state_transition_event{
                                         .kind = graph_state_transition_kind::node_fail,
                                         .cause = frame.cause,
                                         .lifecycle =
                                             graph_node_lifecycle_state::failed,
                                     });
      try_persist_checkpoint();
      release_node_local_state();
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }

    auto branch_status = session.owner_->evaluate_stream_branch_indexed(
        frame.node_id, *stream_output, session.context_, invoke.bound_call_scope);
    if (branch_status.has_error()) {
      session.owner_->publish_node_run_error(invoke.outputs, frame.node_scope.path,
                                             frame.node_id, branch_status.error(),
                                             "branch selector failed");
      session.state_table_.update(frame.node_id,
                                  graph_node_lifecycle_state::failed, 1U,
                                  branch_status.error());
      session.append_transition(frame.node_id, graph_state_transition_event{
                                         .kind = graph_state_transition_kind::node_fail,
                                         .cause = frame.cause,
                                         .lifecycle =
                                             graph_node_lifecycle_state::failed,
                                     });
      try_persist_checkpoint();
      release_node_local_state();
      return wh::core::result<void>::failure(branch_status.error());
    }
    resolved_branch = std::move(branch_status).value();

    auto post_output = detail::fork_graph_reader(*stream_output);
    if (post_output.has_error()) {
      try_persist_checkpoint();
      release_node_local_state();
      return wh::core::result<void>::failure(post_output.error());
    }
    auto post_interrupt = session.owner_->evaluate_interrupt_hook(
        session.context_, invoke.config.interrupt_post_hook, frame.cause.node_key,
        graph_value{std::move(post_output).value()});
    if (post_interrupt.has_error()) {
      post_interrupt_error = post_interrupt.error();
    } else if (post_interrupt.value().has_value()) {
      post_interrupt_signal.emplace(std::move(post_interrupt).value().value());
    }

    auto committed_stream = session.owner_->commit_stream_output(
        frame.node_id, session.io_storage_, std::move(*stream_output),
        resolved_branch);
    if (committed_stream.has_error()) {
      session.owner_->publish_node_run_error(invoke.outputs, frame.node_scope.path,
                                             frame.node_id, committed_stream.error(),
                                             "node output contract mismatch");
      session.state_table_.update(frame.node_id,
                                  graph_node_lifecycle_state::failed, 1U,
                                  committed_stream.error());
      session.append_transition(frame.node_id, graph_state_transition_event{
                                         .kind = graph_state_transition_kind::node_fail,
                                         .cause = frame.cause,
                                         .lifecycle =
                                             graph_node_lifecycle_state::failed,
                                     });
      try_persist_checkpoint();
      release_node_local_state();
      return wh::core::result<void>::failure(committed_stream.error());
    }
  }

  session.state_table_.update(frame.node_id,
                              graph_node_lifecycle_state::completed, 0U,
                              std::nullopt);
  session.append_transition(frame.node_id, graph_state_transition_event{
                                     .kind = graph_state_transition_kind::route_commit,
                                     .cause = frame.cause,
                                     .lifecycle =
                                         graph_node_lifecycle_state::completed,
                                 });
  session.append_transition(frame.node_id, graph_state_transition_event{
                                     .kind = graph_state_transition_kind::node_leave,
                                     .cause = frame.cause,
                                     .lifecycle =
                                         graph_node_lifecycle_state::completed,
                                 });
  if (post_interrupt_error.has_value()) {
    try_persist_checkpoint();
    release_node_local_state();
    return wh::core::result<void>::failure(*post_interrupt_error);
  }
  if (post_interrupt_signal.has_value()) {
    session.context_.interrupt_info =
        wh::compose::to_interrupt_context(std::move(*post_interrupt_signal));
    session.emit_debug(graph_debug_stream_event::decision_kind::interrupt_hit,
                       frame.node_id, invoke.step_count);
    session.request_freeze(false);
    release_node_local_state();
    return wh::core::result<void>::failure(wh::core::errc::canceled);
  }

  if (!session.interrupt_state().freeze_requested) {
    session.owner_->stage_pregel_successors(frame.node_id, resolved_branch,
                                    pregel_delivery_);
    enqueue_fn(frame.node_id);
  }
  release_node_local_state();
  return {};
}

inline auto detail::invoke_runtime::pregel_runtime::commit_skip_action(
    const pregel_action &action) -> wh::core::result<void> {
  session_.state_table_.update(action.node_id,
                               graph_node_lifecycle_state::skipped, 0U,
                               std::nullopt);
  session_.append_transition(action.node_id, graph_state_transition_event{
                                     .kind = graph_state_transition_kind::node_skip,
                                     .cause = action.cause,
                                     .lifecycle =
                                         graph_node_lifecycle_state::skipped,
                                 });
  session_.emit_debug(graph_debug_stream_event::decision_kind::skipped,
                      action.node_id, action.cause.step);
  return {};
}

} // namespace wh::compose
