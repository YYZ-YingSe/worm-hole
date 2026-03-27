#pragma once

#include "wh/compose/graph/graph.hpp"

namespace wh::compose {

inline auto detail::invoke_runtime::run_state::take_next_ready_action()
    -> ready_action {
  if (!owner_->options_.eager && scratch_.ready_head >= current_batch_end_) {
    flush_deferred_enqueue();
    current_batch_end_ = ready_queue().size();
  }
  if (scratch_.ready_head >= ready_queue().size()) {
    return ready_action::no_ready();
  }

  const auto node_id = ready_queue()[scratch_.ready_head++];
  emit_debug(graph_debug_stream_event::decision_kind::dequeue, node_id,
             step_count_);
  queued().clear(node_id);
  if (node_states()[node_id] != node_state::pending) {
    return ready_action::continue_scan();
  }

  const auto &node_key = owner_->runtime_cache_.index.id_to_key[node_id];
  if (!owner_->is_node_designated(node_id, bound_call_scope_)) {
    node_states()[node_id] = node_state::skipped;
    state_table_.update(node_id, graph_node_lifecycle_state::skipped, 0U,
                        std::nullopt);
    append_transition(node_id, graph_state_transition_event{
        .kind = graph_state_transition_kind::node_skip,
        .cause = graph_state_cause{
            .run_id = run_id_,
            .step = step_count_,
            .node_key = node_key,
        },
        .lifecycle = graph_node_lifecycle_state::skipped,
    });
    emit_debug(graph_debug_stream_event::decision_kind::skipped, node_id,
               step_count_);
    auto refreshed_streams = owner_->refresh_source_readers(
        node_id, scratch_, node_states(), branch_states(), context_);
    if (refreshed_streams.has_error()) {
      return ready_action::terminal_error(refreshed_streams.error());
    }
    enqueue_dependents(node_id);
    return ready_action::continue_scan();
  }

  const auto readiness = owner_->classify_node_readiness_indexed(
      node_id, node_states(), branch_states(), output_valid(), scratch_);
  if (readiness == ready_state::waiting) {
    return ready_action::continue_scan();
  }
  if (readiness == ready_state::skipped) {
    node_states()[node_id] = node_state::skipped;
    state_table_.update(node_id, graph_node_lifecycle_state::skipped, 0U,
                        std::nullopt);
    append_transition(node_id, graph_state_transition_event{
        .kind = graph_state_transition_kind::node_skip,
        .cause = graph_state_cause{
            .run_id = run_id_,
            .step = step_count_,
            .node_key = node_key,
        },
        .lifecycle = graph_node_lifecycle_state::skipped,
    });
    emit_debug(graph_debug_stream_event::decision_kind::skipped, node_id,
               step_count_);
    auto refreshed_streams = owner_->refresh_source_readers(
        node_id, scratch_, node_states(), branch_states(), context_);
    if (refreshed_streams.has_error()) {
      return ready_action::terminal_error(refreshed_streams.error());
    }
    enqueue_dependents(node_id);
    return ready_action::continue_scan();
  }

  ++step_count_;
  if (step_count_ > step_budget_) {
    auto completed_nodes = owner_->collect_completed_nodes(node_states());
    invoke_outputs_.last_completed_nodes = completed_nodes;
    invoke_outputs_.step_limit_error = graph_step_limit_error_detail{
        .step = step_count_,
        .budget = step_budget_,
        .node = node_key,
        .completed_nodes = std::move(completed_nodes),
    };
    owner_->publish_graph_run_error(
        invoke_outputs_, runtime_node_path(node_id), node_key,
        compose_error_phase::schedule, wh::core::errc::timeout,
        "step budget exceeded");
    persist_checkpoint_best_effort();
    return ready_action::terminal_error(wh::core::errc::timeout);
  }

  const auto *node = owner_->runtime_cache_.index.nodes_by_id[node_id];
  if (node == nullptr) {
    state_table_.update(node_id, graph_node_lifecycle_state::failed, 1U,
                        wh::core::errc::not_found);
    append_transition(node_id, graph_state_transition_event{
        .kind = graph_state_transition_kind::node_fail,
        .cause = graph_state_cause{
            .run_id = run_id_,
            .step = step_count_,
            .node_key = node_key,
        },
        .lifecycle = graph_node_lifecycle_state::failed,
    });
    persist_checkpoint_best_effort();
    return ready_action::terminal_error(wh::core::errc::not_found);
  }

  auto frame = make_input_frame(node_id, step_count_);
  if (frame.has_error()) {
    state_table_.update(node_id, graph_node_lifecycle_state::failed, 1U,
                        frame.error());
    append_transition(node_id, graph_state_transition_event{
        .kind = graph_state_transition_kind::node_fail,
        .cause = graph_state_cause{
            .run_id = run_id_,
            .step = step_count_,
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
