#pragma once

#include "wh/compose/graph/graph.hpp"
#include "wh/compose/graph/detail/invoke_stage_run.hpp"

namespace wh::compose {

template <typename receiver_t, typename graph_scheduler_t>
class detail::invoke_runtime::run_state::pregel_run
    : public detail::invoke_runtime::run_state::stage_run<
          receiver_t, detail::invoke_runtime::run_state::pregel_run<receiver_t,
                                                                    graph_scheduler_t>,
          graph_scheduler_t> {
  using base_t =
      stage_run<receiver_t, pregel_run<receiver_t, graph_scheduler_t>,
                graph_scheduler_t>;
  friend base_t;

public:
  template <typename graph_scheduler_u>
  pregel_run(run_state &&state, receiver_t &&receiver,
             graph_scheduler_u &&graph_scheduler)
      : base_t(std::move(state), std::move(receiver),
               std::forward<graph_scheduler_u>(graph_scheduler)) {
    this->bind_derived(this);
    const auto node_count = this->state().node_count();
    next_frontier_queued_.reset(node_count, false);
    frontier_ = this->state().ready_queue();
  }

  auto enqueue_committed_node(const std::uint32_t node_id) -> void {
    this->state().enqueue_pregel_dependents(node_id, next_frontier_,
                                            next_frontier_queued_);
  }

  auto begin_superstep() -> wh::core::result<void> {
    if (frontier_.empty()) {
      return {};
    }
    ++this->state().step_count_;
    const auto step = this->state().step_count_;
    if (step > this->state().step_budget_) {
      const auto node_id = frontier_.front();
      const auto &node_key = this->state().node_key(node_id);
      auto completed_nodes = this->state().completed_nodes();
      this->state().invoke_outputs_.last_completed_nodes = completed_nodes;
      this->state().invoke_outputs_.step_limit_error =
          graph_step_limit_error_detail{
              .step = step,
              .budget = this->state().step_budget_,
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
    next_frontier_.clear();
    next_frontier_queued_.reset(this->state().node_count(), false);
    return {};
  }

  auto start_prepared_action(pregel_action action) -> wh::core::result<void> {
    switch (action.action) {
    case pregel_action::kind::waiting:
      return {};
    case pregel_action::kind::skip: {
      auto skipped = this->state().commit_pregel_skip_action(
          action, next_frontier_, next_frontier_queued_);
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

  auto pump() noexcept -> void {
    while (true) {
      if (!this->begin_pump_iteration()) {
        break;
      }

      if (!step_active_) {
        if (frontier_.empty()) {
          this->finish_on_quiescent_boundary();
          break;
        }
        auto begun = begin_superstep();
        if (begun.has_error()) {
          this->finish(
              wh::core::result<graph_value>::failure(begun.error()));
          this->maybe_deliver_finish();
          break;
        }
      }

      while (!this->finish_status_.has_value() &&
             prepared_head_ < prepared_actions_.size() &&
             this->running_async_ < this->state().max_parallel_nodes()) {
        auto started =
            start_prepared_action(std::move(prepared_actions_[prepared_head_++]));
        if (started.has_error()) {
          this->finish(wh::core::result<graph_value>::failure(started.error()));
          break;
        }
      }

      if (!this->result_delivered_ && this->finish_status_.has_value()) {
        this->maybe_deliver_finish();
        break;
      }

      if (this->has_pending_completion()) {
        continue;
      }

      if (prepared_head_ >= prepared_actions_.size() &&
          this->running_async_ == 0U) {
        frontier_ = std::move(next_frontier_);
        next_frontier_.clear();
        prepared_actions_.clear();
        prepared_head_ = 0U;
        step_active_ = false;
        next_frontier_queued_.reset(this->state().node_count(), false);
        continue;
      }

      break;
    }
  }

private:
  std::vector<std::uint32_t> frontier_{};
  std::vector<std::uint32_t> next_frontier_{};
  dynamic_bitset next_frontier_queued_{};
  std::vector<pregel_action> prepared_actions_{};
  std::size_t prepared_head_{0U};
  std::size_t current_step_{0U};
  bool step_active_{false};
};


} // namespace wh::compose
