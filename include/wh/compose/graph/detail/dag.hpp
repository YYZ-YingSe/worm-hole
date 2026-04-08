#pragma once

#include "wh/compose/graph/detail/invoke_stage_run.hpp"
#include "wh/compose/graph/detail/runtime/dag_run_state.hpp"
#include "wh/compose/graph/graph.hpp"
#include "wh/core/compiler.hpp"

namespace wh::compose {

inline auto
detail::invoke_runtime::dag_run_state::make_input_sender(node_frame *frame)
    -> graph_sender {
  return owner_->build_node_input_sender(
      frame->node_id, io_storage_, node_states(), branch_states(), context_,
      frame, invoke_state().config, *invoke_state().graph_scheduler);
}

template <typename receiver_t, typename graph_scheduler_t>
class detail::invoke_runtime::dag_run
    : public detail::invoke_runtime::invoke_stage_run<
          detail::invoke_runtime::dag_run_state, receiver_t,
          detail::invoke_runtime::dag_run<receiver_t, graph_scheduler_t>,
          graph_scheduler_t> {
  using base_t =
      invoke_stage_run<detail::invoke_runtime::dag_run_state, receiver_t,
                       dag_run<receiver_t, graph_scheduler_t>,
                       graph_scheduler_t>;
  friend base_t;

public:
  template <typename graph_scheduler_u>
  dag_run(detail::invoke_runtime::dag_run_state &&state, receiver_t &&receiver,
          graph_scheduler_u &&graph_scheduler)
      : base_t(std::move(state), std::move(receiver),
               std::forward<graph_scheduler_u>(graph_scheduler)) {
    this->bind_derived(this);
  }

  auto enqueue_committed_node(const std::uint32_t node_id) -> void {
    this->state().enqueue_dependents(node_id);
  }

  [[nodiscard]] auto build_input_sender(node_frame *frame) -> graph_sender {
    return this->state().make_input_sender(frame);
  }

  [[nodiscard]] auto build_freeze_sender(const bool external_interrupt)
      -> graph_sender {
    return this->state().make_freeze_sender(
        this->state().capture_dag_pending_inputs(), external_interrupt);
  }

  template <typename enqueue_fn_t>
  auto commit_node_output(node_frame &&frame, graph_value node_output,
                          enqueue_fn_t &&enqueue_fn) -> wh::core::result<void> {
    return this->state().commit_dag_node_output(
        std::move(frame), std::move(node_output),
        std::forward<enqueue_fn_t>(enqueue_fn));
  }

  auto drive() noexcept -> void {
    while (true) {
      if (!this->begin_drive_iteration()) {
        break;
      }

      if (this->state().node_states()[this->state().end_id()] ==
          run_state::node_state::executed) {
        this->finish_on_quiescent_boundary();
        break;
      }

      bool no_ready = false;
      while (!this->finish_status_.has_value() &&
             this->children_.active_count() <
                 this->state().max_parallel_nodes()) {
        auto action = this->state().take_next_ready_action();
        if (action.kind == ready_action_kind::no_ready) {
          no_ready = true;
          break;
        }
        if (action.kind == ready_action_kind::continue_scan) {
          continue;
        }
        if (action.kind == ready_action_kind::terminal_error) {
          this->finish(wh::core::result<graph_value>::failure(action.error));
          break;
        }
        auto started = this->launch_input_stage(std::move(*action.frame));
        if (started.has_error()) {
          this->finish(wh::core::result<graph_value>::failure(started.error()));
          break;
        }
      }

      if (!this->finished() && this->finish_status_.has_value()) {
        this->maybe_deliver_finish();
        break;
      }

      if (this->state().node_states()[this->state().end_id()] ==
          run_state::node_state::executed) {
        this->finish_on_quiescent_boundary();
        break;
      }

      if (no_ready && this->children_.active_count() == 0U) {
        if (this->state().promote_next_wave()) {
          continue;
        }
        this->finish_on_quiescent_boundary();
        break;
      }

      break;
    }
  }

private:
};

} // namespace wh::compose
