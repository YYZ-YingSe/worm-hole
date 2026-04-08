#pragma once

#include "wh/compose/graph/detail/invoke_stage_run.hpp"
#include "wh/compose/graph/detail/runtime/pregel_run_state.hpp"
#include "wh/compose/graph/graph.hpp"

namespace wh::compose {

inline auto
detail::invoke_runtime::pregel_run_state::make_input_sender(node_frame *frame)
    -> graph_sender {
  return owner_->build_pregel_node_input_sender(
      frame->node_id, pregel_delivery_.current[frame->node_id], io_storage_,
      context_, frame, invoke_state().config, *invoke_state().graph_scheduler);
}

template <typename receiver_t, typename graph_scheduler_t>
class detail::invoke_runtime::pregel_run
    : public detail::invoke_runtime::invoke_stage_run<
          detail::invoke_runtime::pregel_run_state, receiver_t,
          detail::invoke_runtime::pregel_run<receiver_t, graph_scheduler_t>,
          graph_scheduler_t> {
  using base_t =
      invoke_stage_run<detail::invoke_runtime::pregel_run_state, receiver_t,
                       pregel_run<receiver_t, graph_scheduler_t>,
                       graph_scheduler_t>;
  friend base_t;

public:
  template <typename graph_scheduler_u>
  pregel_run(detail::invoke_runtime::pregel_run_state &&state,
             receiver_t &&receiver, graph_scheduler_u &&graph_scheduler)
      : base_t(std::move(state), std::move(receiver),
               std::forward<graph_scheduler_u>(graph_scheduler)) {
    this->bind_derived(this);
    frontier_ = this->state().pregel_delivery().current_frontier();
  }

  auto enqueue_committed_node(const std::uint32_t) -> void {}

  [[nodiscard]] auto build_input_sender(node_frame *frame) -> graph_sender {
    return this->state().make_input_sender(frame);
  }

  [[nodiscard]] auto build_freeze_sender(const bool external_interrupt)
      -> graph_sender {
    return this->state().make_freeze_sender(
        this->state().capture_pregel_pending_inputs(), external_interrupt);
  }

  template <typename enqueue_fn_t>
  auto commit_node_output(node_frame &&frame, graph_value node_output,
                          enqueue_fn_t &&enqueue_fn) -> wh::core::result<void> {
    return this->state().commit_pregel_node_output(
        std::move(frame), std::move(node_output),
        std::forward<enqueue_fn_t>(enqueue_fn));
  }

  auto begin_superstep() -> wh::core::result<void> {
    if (frontier_.empty()) {
      return {};
    }
    ++this->state().invoke_state().step_count;
    const auto step = this->state().invoke_state().step_count;
    if (step > this->state().invoke_state().step_budget) {
      const auto node_id = frontier_.front();
      const auto &node_key = this->state().node_key(node_id);
      auto completed_nodes = this->state().completed_nodes();
      this->state().invoke_state().outputs.last_completed_nodes =
          completed_nodes;
      this->state().invoke_state().outputs.step_limit_error =
          graph_step_limit_error_detail{
              .step = step,
              .budget = this->state().invoke_state().step_budget,
              .node = node_key,
              .completed_nodes = std::move(completed_nodes),
          };
      this->state().publish_graph_error(
          this->state().runtime_node_path(node_id), node_key,
          compose_error_phase::schedule, wh::core::errc::timeout,
          "step budget exceeded");
      this->state().persist_checkpoint_best_effort();
      return wh::core::result<void>::failure(wh::core::errc::timeout);
    }

    prepared_actions_.clear();
    prepared_actions_.reserve(frontier_.size());
    current_step_ = step;
    for (const auto node_id : frontier_) {
      prepared_actions_.push_back(
          this->state().take_next_pregel_action(node_id, step));
    }
    prepared_head_ = 0U;
    step_active_ = true;
    return {};
  }

  auto start_prepared_action(pregel_action action) -> wh::core::result<void> {
    switch (action.action) {
    case pregel_action::kind::waiting:
      return {};
    case pregel_action::kind::skip: {
      auto skipped = this->state().commit_pregel_skip_action(action);
      if (skipped.has_error()) {
        return wh::core::result<void>::failure(skipped.error());
      }
      return {};
    }
    case pregel_action::kind::terminal_error:
      return wh::core::result<void>::failure(action.error);
    case pregel_action::kind::launch:
      return this->launch_input_stage(std::move(*action.frame));
    }
    return {};
  }

  auto drive() noexcept -> void {
    while (true) {
      if (!this->begin_drive_iteration()) {
        break;
      }

      if (!step_active_) {
        if (frontier_.empty()) {
          this->finish_on_quiescent_boundary();
          break;
        }
        auto begun = begin_superstep();
        if (begun.has_error()) {
          this->finish(wh::core::result<graph_value>::failure(begun.error()));
          this->maybe_deliver_finish();
          break;
        }
      }

      while (!this->finish_status_.has_value() &&
             prepared_head_ < prepared_actions_.size() &&
             this->children_.active_count() <
                 this->state().max_parallel_nodes()) {
        auto started = start_prepared_action(
            std::move(prepared_actions_[prepared_head_++]));
        if (started.has_error()) {
          this->finish(wh::core::result<graph_value>::failure(started.error()));
          break;
        }
      }

      if (!this->finished() && this->finish_status_.has_value()) {
        this->maybe_deliver_finish();
        break;
      }

      if (this->completion_ready()) {
        continue;
      }

      if (prepared_head_ >= prepared_actions_.size() &&
          this->children_.active_count() == 0U) {
        frontier_ = this->state().pregel_delivery().advance_superstep();
        prepared_actions_.clear();
        prepared_head_ = 0U;
        step_active_ = false;
        continue;
      }

      break;
    }
  }

private:
  std::vector<std::uint32_t> frontier_{};
  std::vector<pregel_action> prepared_actions_{};
  std::size_t prepared_head_{0U};
  std::size_t current_step_{0U};
  bool step_active_{false};
};

} // namespace wh::compose
