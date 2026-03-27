#pragma once

#include "wh/compose/graph/graph.hpp"
#include "wh/compose/graph/detail/invoke_join.hpp"

namespace wh::compose {

template <typename receiver_t, typename lane_scheduler_t>
class detail::invoke_runtime::run_state::pregel_run
    : public detail::invoke_runtime::run_state::join_base<
          receiver_t,
          detail::invoke_runtime::run_state::pregel_run<receiver_t,
                                                           lane_scheduler_t>,
          lane_scheduler_t> {
      using base_t =
          join_base<receiver_t, pregel_run<receiver_t, lane_scheduler_t>,
                           lane_scheduler_t>;
      friend base_t;

    public:
      template <typename lane_scheduler_u>
      pregel_run(run_state &&state, receiver_t &&receiver,
                     lane_scheduler_u &&lane_scheduler)
          : base_t(state.node_count() + 1U,
                   wh::core::detail::any_resume_scheduler_t{
                       *state.resume_scheduler_},
                   lane_scheduler_t{std::forward<lane_scheduler_u>(
                       lane_scheduler)}) {
        state_.emplace(std::move(state));
        const auto node_count = state_->node_count();
        next_frontier_queued_.reset(node_count, false);
        frontier_ = state_->ready_queue();
        this->emplace_receiver(std::move(receiver));
      }

      auto bind_outer_stop(
          const std::shared_ptr<pregel_run> &self) noexcept -> void {
        base_t::bind_outer_stop(self);
      }

      auto request_pump(const std::shared_ptr<pregel_run> &self) noexcept
          -> void {
        base_t::request_pump(self);
      }

    private:
      auto prepare_finish_delivery() noexcept -> void {
        if (!state_) {
          return;
        }
        state_->publish_runtime_outputs();
        state_.reset();
      }

      auto release_frame(node_frame &frame) noexcept -> void {
        frame.node_local_scope.release(state_->node_local_process_states_);
      }

      auto launch_input_stage(node_frame &&frame,
                              const std::shared_ptr<pregel_run> &self)
          -> wh::core::result<void> {
        return this->start_child(
            state_->build_input_sender(std::addressof(frame)),
            std::move(frame), self);
      }

      auto launch_state_stage(node_frame &&frame,
                              graph_sender sender,
                              const std::shared_ptr<pregel_run> &self)
          -> wh::core::result<void> {
        return this->start_child(std::move(sender), std::move(frame), self);
      }

      auto launch_freeze_stage(const std::shared_ptr<pregel_run> &self)
          -> wh::core::result<void> {
        node_frame frame{};
        frame.stage = invoke_stage::freeze;
        frame.node_id = state_->control_slot_id();
        return this->start_child(
            state_->freeze_sender(state_->freeze_external()), std::move(frame),
            self);
      }

      auto launch_node_stage(node_frame &&frame,
                             const std::shared_ptr<pregel_run> &self)
          -> wh::core::result<void> {
        if (frame.node == nullptr) {
          return wh::core::result<void>::failure(wh::core::errc::not_found);
        }
        if (!state_->should_retain_input(frame)) {
          if (!frame.node_input.has_value()) {
            return wh::core::result<void>::failure(wh::core::errc::not_found);
          }
          if (compiled_node_is_sync(*frame.node)) {
            auto executed = run_sync_node_execution(
                *frame.node, *frame.node_input, state_->context_,
                state_->bound_call_scope_, std::addressof(*state_), frame,
                [this](const std::uint32_t node_id,
                       const std::size_t step) noexcept {
                  state_->emit_debug(
                      graph_debug_stream_event::decision_kind::retry, node_id,
                      step);
                });
            return settle_node_stage(std::move(frame), std::move(executed), self);
          }
          return this->start_child(
              make_async_node_attempt_sender(
                  *frame.node, *frame.node_input, state_->context_,
                  state_->bound_call_scope_, std::addressof(*state_), frame),
              std::move(frame), self);
        }

        auto *rerun_input = state_->rerun_state_.find(frame.node_id);
        if (rerun_input == nullptr) {
          return wh::core::result<void>::failure(wh::core::errc::not_found);
        }
        auto execution_input = fork_graph_value(*rerun_input);
        if (execution_input.has_error()) {
          return wh::core::result<void>::failure(execution_input.error());
        }
        if (compiled_node_is_sync(*frame.node)) {
          auto live_input = std::move(execution_input).value();
          auto executed = run_sync_node_execution(
              *frame.node, live_input, state_->context_,
              state_->bound_call_scope_, std::addressof(*state_), frame,
              [this](const std::uint32_t node_id,
                     const std::size_t step) noexcept {
                state_->emit_debug(
                    graph_debug_stream_event::decision_kind::retry, node_id,
                    step);
              });
          return settle_node_stage(std::move(frame), std::move(executed), self);
        }
        frame.node_input.emplace(std::move(execution_input).value());
        return this->start_child(
            make_async_node_attempt_sender(
                *frame.node, *frame.node_input, state_->context_,
                state_->bound_call_scope_, std::addressof(*state_), frame),
            std::move(frame), self);
      }

      auto continue_node_stage(node_frame &&frame,
                               graph_value input,
                               const std::shared_ptr<pregel_run> &self)
          -> wh::core::result<void> {
        auto prepared_input =
            state_->prepare_execution_input(std::move(frame), std::move(input));
        if (prepared_input.has_error()) {
          return wh::core::result<void>::failure(prepared_input.error());
        }
        auto prepared_stage = std::move(prepared_input).value();
        if (prepared_stage.sender.has_value()) {
          return launch_state_stage(std::move(prepared_stage.frame),
                                    std::move(*prepared_stage.sender), self);
        }

        auto next =
            state_->finalize_node_frame(std::move(prepared_stage.frame),
                                        std::move(prepared_stage.payload));
        if (next.has_error()) {
          return wh::core::result<void>::failure(next.error());
        }

        auto prepared = std::move(next).value();
        if (state_->cache_store_ != nullptr && prepared.cache_key.has_value()) {
          const auto cache_iter =
              state_->cache_store_->find(*prepared.cache_key);
          if (cache_iter != state_->cache_store_->end()) {
            return settle_node_stage(
                std::move(prepared),
                wh::core::result<graph_value>{graph_value{cache_iter->second}},
                self);
          }
        }
        return launch_node_stage(std::move(prepared), self);
      }

      auto settle_pre_state_stage(
          node_frame &&frame,
          wh::core::result<graph_value> &&resolved,
          const std::shared_ptr<pregel_run> &self)
          -> wh::core::result<void> {
        if (resolved.has_error()) {
          return state_->fail_node_stage(std::move(frame), resolved.error(),
                                         "node pre-state handler failed");
        }
        return continue_node_stage(std::move(frame), std::move(resolved).value(),
                                   self);
      }

      auto settle_prepare_stage(
          node_frame &&frame,
          wh::core::result<graph_value> &&resolved,
          const std::shared_ptr<pregel_run> &self)
          -> wh::core::result<void> {
        if (resolved.has_error()) {
          return state_->fail_node_stage(
              std::move(frame), resolved.error(),
              "node execution input normalization failed");
        }
        return continue_node_stage(std::move(frame), std::move(resolved).value(),
                                   self);
      }

      auto settle_post_state_stage(
          node_frame &&frame,
          wh::core::result<graph_value> &&resolved)
          -> wh::core::result<void> {
        if (resolved.has_error()) {
          return state_->fail_node_stage(std::move(frame), resolved.error(),
                                         "node post-state handler failed");
        }
        return state_->commit_node_output(
            std::move(frame), std::move(resolved).value(),
            [this](const std::uint32_t node_id) {
              state_->enqueue_pregel_dependents(node_id, next_frontier_,
                                                next_frontier_queued_);
            });
      }

      auto settle_node_stage(
          node_frame &&frame,
          wh::core::result<graph_value> &&executed,
          const std::shared_ptr<pregel_run> &self)
          -> wh::core::result<void> {
        if (executed.has_error()) {
          if (!state_->freeze_requested() && frame.attempt < frame.retry_budget) {
            state_->emit_debug(graph_debug_stream_event::decision_kind::retry,
                               frame.node_id, frame.cause.step);
            ++frame.attempt;
            return launch_node_stage(std::move(frame), self);
          }

          const auto error_code = executed.error();
          if (state_->should_wrap_node_error(error_code)) {
            return state_->fail_node_stage(std::move(frame), error_code,
                                           "node execution failed");
          }
          state_->state_table_.update(frame.node_id,
                                      graph_node_lifecycle_state::canceled,
                                      frame.retry_budget + 1U, error_code);
          state_->append_transition(frame.node_id, graph_state_transition_event{
              .kind = graph_state_transition_kind::node_leave,
              .cause = frame.cause,
              .lifecycle = graph_node_lifecycle_state::canceled,
          });
          state_->persist_checkpoint_best_effort();
          frame.node_local_scope.release(state_->node_local_process_states_);
          return wh::core::result<void>::failure(error_code);
        }

        auto post = state_->begin_state_post(std::move(frame),
                                             std::move(executed).value());
        if (post.has_error()) {
          return wh::core::result<void>::failure(post.error());
        }
        auto stage = std::move(post).value();
        if (stage.sender.has_value()) {
          return launch_state_stage(std::move(stage.frame),
                                    std::move(*stage.sender), self);
        }
        return state_->commit_node_output(
            std::move(stage.frame), std::move(stage.payload),
            [this](const std::uint32_t node_id) {
              state_->enqueue_pregel_dependents(node_id, next_frontier_,
                                                next_frontier_queued_);
            });
      }

      auto settle_input_stage(
          node_frame &&frame,
          wh::core::result<graph_value> &&resolved,
          const std::shared_ptr<pregel_run> &self)
          -> wh::core::result<void> {
        if (resolved.has_error()) {
          state_->publish_node_error(
              state_->runtime_node_path(frame.node_id),
              frame.node_id, resolved.error(), "node input resolution failed");
          state_->state_table_.update(frame.node_id,
                                      graph_node_lifecycle_state::failed, 1U,
                                      resolved.error());
          state_->append_transition(frame.node_id, graph_state_transition_event{
              .kind = graph_state_transition_kind::node_fail,
              .cause = frame.cause,
              .lifecycle = graph_node_lifecycle_state::failed,
          });
          state_->persist_checkpoint_best_effort();
          return wh::core::result<void>::failure(resolved.error());
        }
        auto input = std::move(resolved).value();

        if (frame.node_id == state_->end_id()) {
          auto stored = state_->store_output(frame.node_id, std::move(input));
          if (stored.has_error()) {
            state_->persist_checkpoint_best_effort();
            return wh::core::result<void>::failure(stored.error());
          }
          state_->node_states()[frame.node_id] = node_state::executed;
          state_->state_table_.update(frame.node_id,
                                      graph_node_lifecycle_state::completed, 0U,
                                      std::nullopt);
          state_->append_transition(frame.node_id, graph_state_transition_event{
              .kind = graph_state_transition_kind::route_commit,
              .cause = frame.cause,
              .lifecycle = graph_node_lifecycle_state::completed,
          });
          state_->enqueue_pregel_dependents(frame.node_id, next_frontier_,
                                            next_frontier_queued_);
          return {};
        }

        auto prepared = state_->begin_state_pre(
            std::move(frame), std::move(input));
        if (prepared.has_error()) {
          if (prepared.error() == wh::core::errc::canceled &&
              state_->freeze_requested()) {
            return {};
          }
          return wh::core::result<void>::failure(prepared.error());
        }

        auto stage = std::move(prepared).value();
        if (stage.sender.has_value()) {
          return launch_state_stage(std::move(stage.frame), std::move(*stage.sender),
                                    self);
        }
        return continue_node_stage(std::move(stage.frame),
                                   std::move(stage.payload), self);
      }

      auto settle_async_node(
          node_frame &&frame,
          wh::core::result<graph_value> &&executed,
          const std::shared_ptr<pregel_run> &self)
          -> wh::core::result<void> {
        if (frame.stage == invoke_stage::input) {
          return settle_input_stage(std::move(frame), std::move(executed), self);
        }
        if (frame.stage == invoke_stage::pre_state) {
          return settle_pre_state_stage(std::move(frame), std::move(executed), self);
        }
        if (frame.stage == invoke_stage::prepare) {
          return settle_prepare_stage(std::move(frame), std::move(executed), self);
        }
        if (frame.stage == invoke_stage::post_state) {
          return settle_post_state_stage(std::move(frame), std::move(executed));
        }
        if (frame.stage == invoke_stage::freeze) {
          if (executed.has_error()) {
            return wh::core::result<void>::failure(executed.error());
          }
          this->finish(
              wh::core::result<graph_value>::failure(wh::core::errc::canceled),
              self);
          return {};
        }
        return settle_node_stage(std::move(frame), std::move(executed), self);
      }

      auto begin_superstep() -> wh::core::result<void> {
        if (frontier_.empty()) {
          return {};
        }
        ++state_->step_count_;
        const auto step = state_->step_count_;
        if (step > state_->step_budget_) {
          const auto node_id = frontier_.front();
          const auto &node_key = state_->node_key(node_id);
          auto completed_nodes = state_->completed_nodes();
          state_->invoke_outputs_.last_completed_nodes = completed_nodes;
          state_->invoke_outputs_.step_limit_error = graph_step_limit_error_detail{
              .step = step,
              .budget = state_->step_budget_,
              .node = node_key,
              .completed_nodes = std::move(completed_nodes),
          };
          state_->publish_graph_error(
              state_->runtime_node_path(node_id),
              node_key, compose_error_phase::schedule, wh::core::errc::timeout,
              "step budget exceeded");
          state_->persist_checkpoint_best_effort();
          return wh::core::result<void>::failure(wh::core::errc::timeout);
        }

        prepared_actions_.clear();
        prepared_actions_.reserve(frontier_.size());
        current_step_ = step;
        for (const auto node_id : frontier_) {
          prepared_actions_.push_back(state_->take_next_pregel_action(node_id, step));
        }
        prepared_head_ = 0U;
        step_active_ = true;
        next_frontier_.clear();
        next_frontier_queued_.reset(state_->node_count(), false);
        return {};
      }

      auto drain_completions(
          const std::shared_ptr<pregel_run> &self) noexcept -> void {
        base_t::drain_completions(
            self,
            [this](node_frame &frame) noexcept {
              release_frame(frame);
            },
            [this](node_frame &&frame,
                   wh::core::result<graph_value> &&executed,
                   const std::shared_ptr<pregel_run> &shared)
                -> wh::core::result<void> {
              return settle_async_node(std::move(frame), std::move(executed),
                                       shared);
            });
      }

      auto start_prepared_action(pregel_action action,
                                 const std::shared_ptr<pregel_run> &self)
          -> wh::core::result<void> {
        switch (action.action) {
        case pregel_action::kind::waiting:
          return {};
        case pregel_action::kind::skip:
          {
            auto skipped = state_->commit_pregel_skip_action(
                action, next_frontier_, next_frontier_queued_);
            if (skipped.has_error()) {
              return wh::core::result<void>::failure(skipped.error());
            }
          }
          return {};
        case pregel_action::kind::terminal_error:
          return wh::core::result<void>::failure(action.error);
        case pregel_action::kind::launch:
          return launch_input_stage(std::move(*action.frame), self);
        }
        return {};
      }

      auto pump(const std::shared_ptr<pregel_run> &self) noexcept -> void {
        while (true) {
          this->poll_outer_stop(self);
          drain_completions(self);
          if (!this->result_delivered_ && this->finish_status_.has_value()) {
            this->maybe_deliver_finish();
          }
          if (this->result_delivered_) {
            break;
          }

          auto boundary_interrupt = state_->check_external_interrupt_boundary();
          if (boundary_interrupt.has_error()) {
            this->finish(
                wh::core::result<graph_value>::failure(
                    boundary_interrupt.error()),
                self);
            this->maybe_deliver_finish();
            break;
          }
          if (state_->freeze_requested()) {
            if (this->running_async_ == 0U) {
              auto started = launch_freeze_stage(self);
              if (started.has_error()) {
                this->finish(
                    wh::core::result<graph_value>::failure(started.error()),
                    self);
                this->maybe_deliver_finish();
              }
            }
            break;
          }

          if (!step_active_) {
            if (frontier_.empty()) {
              if (state_->external_interrupt_wait_mode_active_) {
                state_->request_freeze(true);
                auto started = launch_freeze_stage(self);
                if (started.has_error()) {
                  this->finish(
                      wh::core::result<graph_value>::failure(started.error()),
                      self);
                  this->maybe_deliver_finish();
                }
                break;
              }
              this->finish(state_->finish_graph_status(), self);
              this->maybe_deliver_finish();
              break;
            }
            auto begun = begin_superstep();
            if (begun.has_error()) {
              this->finish(
                  wh::core::result<graph_value>::failure(begun.error()), self);
              this->maybe_deliver_finish();
              break;
            }
          }

          while (!this->finish_status_.has_value() &&
                 prepared_head_ < prepared_actions_.size() &&
                 this->running_async_ < state_->max_parallel_nodes()) {
            auto started =
                start_prepared_action(std::move(prepared_actions_[prepared_head_++]),
                                      self);
            if (started.has_error()) {
              this->finish(
                  wh::core::result<graph_value>::failure(started.error()), self);
              break;
            }
          }

          if (!this->result_delivered_ && this->finish_status_.has_value()) {
            this->maybe_deliver_finish();
            break;
          }

          if (this->completion_head_.load(std::memory_order_acquire) !=
              base_t::no_slot_) {
            continue;
          }

          if (prepared_head_ >= prepared_actions_.size() &&
              this->running_async_ == 0U) {
            frontier_ = std::move(next_frontier_);
            next_frontier_.clear();
            prepared_actions_.clear();
            prepared_head_ = 0U;
            step_active_ = false;
            next_frontier_queued_.reset(state_->node_count(), false);
            continue;
          }

          break;
        }
      }

      std::optional<run_state> state_{};
      std::vector<std::uint32_t> frontier_{};
      std::vector<std::uint32_t> next_frontier_{};
      dynamic_bitset next_frontier_queued_{};
      std::vector<pregel_action> prepared_actions_{};
      std::size_t prepared_head_{0U};
      std::size_t current_step_{0U};
      bool step_active_{false};
    };


} // namespace wh::compose
