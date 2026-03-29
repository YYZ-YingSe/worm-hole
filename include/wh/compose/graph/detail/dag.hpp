#pragma once

#include "wh/compose/graph/graph.hpp"
#include "wh/compose/graph/detail/invoke_stage_run.hpp"

namespace wh::compose {

template <typename receiver_t, typename graph_scheduler_t>
class detail::invoke_runtime::run_state::dag_run
    : public detail::invoke_runtime::run_state::stage_run<
          receiver_t, detail::invoke_runtime::run_state::dag_run<receiver_t,
                                                                 graph_scheduler_t>,
          graph_scheduler_t> {
  using base_t =
      stage_run<receiver_t, dag_run<receiver_t, graph_scheduler_t>,
                graph_scheduler_t>;
  friend base_t;

public:
  template <typename graph_scheduler_u>
  dag_run(run_state &&state, receiver_t &&receiver,
          graph_scheduler_u &&graph_scheduler)
      : base_t(std::move(state), std::move(receiver),
               std::forward<graph_scheduler_u>(graph_scheduler)) {
    this->bind_derived(this);
  }

  auto enqueue_committed_node(const std::uint32_t node_id) -> void {
    this->state().enqueue_dependents(node_id);
  }

  auto pump() noexcept -> void {
    while (true) {
      if (!this->begin_pump_iteration()) {
        break;
      }

      if (this->state().node_states()[this->state().end_id()] ==
          node_state::executed) {
        this->finish_on_quiescent_boundary();
        break;
      }

      bool no_ready = false;
      while (!this->finish_status_.has_value() &&
             this->running_async_ < this->state().max_parallel_nodes()) {
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

      if (!this->result_delivered_ && this->finish_status_.has_value()) {
        this->maybe_deliver_finish();
        break;
      }

      if (this->state().node_states()[this->state().end_id()] ==
          node_state::executed) {
        this->finish_on_quiescent_boundary();
        break;
      }

      if (no_ready && this->running_async_ == 0U) {
        this->finish_on_quiescent_boundary();
        break;
      }

      break;
    }
  }

private:
};


} // namespace wh::compose
