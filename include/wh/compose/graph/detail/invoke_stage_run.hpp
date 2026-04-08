// Defines the shared node-stage execution skeleton used by both DAG and
// Pregel graph runtimes.
#pragma once

#include "wh/compose/graph/detail/invoke_join.hpp"
#include "wh/compose/graph/graph.hpp"
#include "wh/core/compiler.hpp"

namespace wh::compose {

template <typename state_t, typename receiver_t, typename derived_t,
          typename graph_scheduler_t>
class detail::invoke_runtime::invoke_stage_run
    : public detail::invoke_runtime::invoke_join_base<receiver_t, derived_t,
                                                      graph_scheduler_t> {
  using join_base_t =
      invoke_join_base<receiver_t, derived_t, graph_scheduler_t>;

public:
  template <typename receiver_arg_t, typename graph_scheduler_u>
  invoke_stage_run(state_t &&state, receiver_arg_t &&receiver,
                   graph_scheduler_u &&graph_scheduler)
      : join_base_t(
            state.node_count() + 1U,
            graph_scheduler_t{std::forward<graph_scheduler_u>(graph_scheduler)},
            std::forward<receiver_arg_t>(receiver)) {
    state_.emplace(std::move(state));
    state_->rebind_moved_runtime_storage();
  }

  auto bind_outer_stop() noexcept -> void { join_base_t::bind_outer_stop(); }

  auto request_drive() noexcept -> void { join_base_t::request_drive(); }

protected:
  [[nodiscard]] auto state() noexcept -> state_t & { return *state_; }

  [[nodiscard]] auto state() const noexcept -> const state_t & {
    return *state_;
  }

public:
  auto start() noexcept -> void { join_base_t::start(); }

  auto prepare_finish_delivery() noexcept -> void {
    if (!state_.has_value()) {
      return;
    }
    state_->publish_runtime_outputs();
    state_.reset();
  }

protected:
  auto release_frame(node_frame &frame) noexcept -> void {
    frame.node_local_scope.release(state_->node_local_process_states_);
  }

  auto launch_input_stage(node_frame &&frame) -> wh::core::result<void> {
    return this->start_child(static_cast<derived_t &>(*this).build_input_sender(
                                 std::addressof(frame)),
                             std::move(frame));
  }

  auto launch_state_stage(node_frame &&frame, graph_sender sender)
      -> wh::core::result<void> {
    return this->start_child(std::move(sender), std::move(frame));
  }

  auto launch_freeze_stage() -> wh::core::result<void> {
    node_frame frame{};
    frame.stage = stage::freeze;
    frame.node_id = state_->control_slot_id();
    return this->start_child(
        static_cast<derived_t &>(*this).build_freeze_sender(
            state_->freeze_external()),
        std::move(frame));
  }

  auto launch_node_stage(node_frame &&frame) -> wh::core::result<void> {
    if (frame.node == nullptr) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    if (!state_->should_retain_input(frame)) {
      if (!frame.node_input.has_value()) {
        return wh::core::result<void>::failure(wh::core::errc::not_found);
      }
      if (compiled_node_is_sync(*frame.node)) {
        auto executed = run_state::run_sync_node_execution(
            *frame.node, *frame.node_input, state_->context_,
            state_->invoke_state().bound_call_scope, std::addressof(*state_),
            frame,
            [this](const std::uint32_t node_id,
                   const std::size_t step) noexcept {
              state_->emit_debug(graph_debug_stream_event::decision_kind::retry,
                                 node_id, step);
            });
        return settle_node_stage(std::move(frame), std::move(executed));
      }
      return this->start_child(run_state::make_async_node_attempt_sender(
                                   *frame.node, *frame.node_input,
                                   state_->context_,
                                   state_->invoke_state().bound_call_scope,
                                   std::addressof(*state_), frame),
                               std::move(frame));
    }

    auto *rerun_input = state_->rerun_state_.find(frame.node_id);
    if (rerun_input == nullptr) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    auto execution_input =
        frame.node->meta.input_contract == node_contract::stream
            ? detail::fork_graph_reader_payload(*rerun_input)
            : fork_graph_value(*rerun_input);
    if (execution_input.has_error()) {
      return wh::core::result<void>::failure(execution_input.error());
    }
    if (compiled_node_is_sync(*frame.node)) {
      auto live_input = std::move(execution_input).value();
      auto executed = run_state::run_sync_node_execution(
          *frame.node, live_input, state_->context_,
          state_->invoke_state().bound_call_scope, std::addressof(*state_),
          frame,
          [this](const std::uint32_t node_id, const std::size_t step) noexcept {
            state_->emit_debug(graph_debug_stream_event::decision_kind::retry,
                               node_id, step);
          });
      return settle_node_stage(std::move(frame), std::move(executed));
    }
    frame.node_input.emplace(std::move(execution_input).value());
    return this->start_child(run_state::make_async_node_attempt_sender(
                                 *frame.node, *frame.node_input,
                                 state_->context_,
                                 state_->invoke_state().bound_call_scope,
                                 std::addressof(*state_), frame),
                             std::move(frame));
  }

  auto continue_node_stage(node_frame &&frame, graph_value input)
      -> wh::core::result<void> {
    auto prepared_input =
        state_->prepare_execution_input(std::move(frame), std::move(input));
    if (prepared_input.has_error()) {
      return wh::core::result<void>::failure(prepared_input.error());
    }
    auto prepared_stage = std::move(prepared_input).value();
    if (prepared_stage.sender.has_value()) {
      return launch_state_stage(std::move(prepared_stage.frame),
                                std::move(*prepared_stage.sender));
    }

    auto next = state_->finalize_node_frame(std::move(prepared_stage.frame),
                                            std::move(prepared_stage.payload));
    if (next.has_error()) {
      return wh::core::result<void>::failure(next.error());
    }

    auto prepared = std::move(next).value();
    return launch_node_stage(std::move(prepared));
  }

  auto settle_pre_state_stage(node_frame &&frame,
                              wh::core::result<graph_value> &&resolved)
      -> wh::core::result<void> {
    if (resolved.has_error()) {
      return state_->fail_node_stage(std::move(frame), resolved.error(),
                                     "node pre-state handler failed");
    }
    return continue_node_stage(std::move(frame), std::move(resolved).value());
  }

  auto settle_prepare_stage(node_frame &&frame,
                            wh::core::result<graph_value> &&resolved)
      -> wh::core::result<void> {
    if (resolved.has_error()) {
      return state_->fail_node_stage(
          std::move(frame), resolved.error(),
          "node execution input normalization failed");
    }
    return continue_node_stage(std::move(frame), std::move(resolved).value());
  }

  auto settle_post_state_stage(node_frame &&frame,
                               wh::core::result<graph_value> &&resolved)
      -> wh::core::result<void> {
    if (resolved.has_error()) {
      return state_->fail_node_stage(std::move(frame), resolved.error(),
                                     "node post-state handler failed");
    }
    return static_cast<derived_t &>(*this).commit_node_output(
        std::move(frame), std::move(resolved).value(),
        [this](const std::uint32_t node_id) {
          static_cast<derived_t &>(*this).enqueue_committed_node(node_id);
        });
  }

  auto settle_node_stage(node_frame &&frame,
                         wh::core::result<graph_value> &&executed)
      -> wh::core::result<void> {
    if (executed.has_error()) {
      if (!state_->freeze_requested() && frame.attempt < frame.retry_budget) {
        state_->emit_debug(graph_debug_stream_event::decision_kind::retry,
                           frame.node_id, frame.cause.step);
        ++frame.attempt;
        return launch_node_stage(std::move(frame));
      }

      const auto error_code = executed.error();
      if (state_->should_wrap_node_error(error_code)) {
        return state_->fail_node_stage(std::move(frame), error_code,
                                       "node execution failed");
      }
      state_->state_table_.update(frame.node_id,
                                  graph_node_lifecycle_state::canceled,
                                  frame.retry_budget + 1U, error_code);
      state_->append_transition(
          frame.node_id, graph_state_transition_event{
                             .kind = graph_state_transition_kind::node_leave,
                             .cause = frame.cause,
                             .lifecycle = graph_node_lifecycle_state::canceled,
                         });
      state_->persist_checkpoint_best_effort();
      frame.node_local_scope.release(state_->node_local_process_states_);
      return wh::core::result<void>::failure(error_code);
    }

    auto post =
        state_->begin_state_post(std::move(frame), std::move(executed).value());
    if (post.has_error()) {
      return wh::core::result<void>::failure(post.error());
    }
    auto stage = std::move(post).value();
    if (stage.sender.has_value()) {
      return launch_state_stage(std::move(stage.frame),
                                std::move(*stage.sender));
    }
    return static_cast<derived_t &>(*this).commit_node_output(
        std::move(stage.frame), std::move(stage.payload),
        [this](const std::uint32_t node_id) {
          static_cast<derived_t &>(*this).enqueue_committed_node(node_id);
        });
  }

  auto settle_input_stage(node_frame &&frame,
                          wh::core::result<graph_value> &&resolved)
      -> wh::core::result<void> {
    if (resolved.has_error()) {
      state_->publish_node_error(state_->runtime_node_path(frame.node_id),
                                 frame.node_id, resolved.error(),
                                 "node input resolution failed");
      state_->state_table_.update(frame.node_id,
                                  graph_node_lifecycle_state::failed, 1U,
                                  resolved.error());
      state_->append_transition(
          frame.node_id, graph_state_transition_event{
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
      state_->node_states()[frame.node_id] = run_state::node_state::executed;
      state_->state_table_.update(frame.node_id,
                                  graph_node_lifecycle_state::completed, 0U,
                                  std::nullopt);
      state_->append_transition(
          frame.node_id, graph_state_transition_event{
                             .kind = graph_state_transition_kind::route_commit,
                             .cause = frame.cause,
                             .lifecycle = graph_node_lifecycle_state::completed,
                         });
      static_cast<derived_t &>(*this).enqueue_committed_node(frame.node_id);
      return {};
    }

    auto prepared = state_->begin_state_pre(std::move(frame), std::move(input));
    if (prepared.has_error()) {
      if (prepared.error() == wh::core::errc::canceled &&
          state_->freeze_requested()) {
        return {};
      }
      return wh::core::result<void>::failure(prepared.error());
    }

    auto stage = std::move(prepared).value();
    if (stage.sender.has_value()) {
      return launch_state_stage(std::move(stage.frame),
                                std::move(*stage.sender));
    }
    return continue_node_stage(std::move(stage.frame),
                               std::move(stage.payload));
  }

  auto settle_async_node(node_frame &&frame,
                         wh::core::result<graph_value> &&executed)
      -> wh::core::result<void> {
    if (frame.stage == stage::input) {
      return settle_input_stage(std::move(frame), std::move(executed));
    }
    if (frame.stage == stage::pre_state) {
      return settle_pre_state_stage(std::move(frame), std::move(executed));
    }
    if (frame.stage == stage::prepare) {
      return settle_prepare_stage(std::move(frame), std::move(executed));
    }
    if (frame.stage == stage::post_state) {
      return settle_post_state_stage(std::move(frame), std::move(executed));
    }
    if (frame.stage == stage::freeze) {
      if (executed.has_error()) {
        return wh::core::result<void>::failure(executed.error());
      }
      this->finish(
          wh::core::result<graph_value>::failure(wh::core::errc::canceled));
      return {};
    }
    wh_invariant(frame.stage == stage::node);
    return settle_node_stage(std::move(frame), std::move(executed));
  }

  auto drain_runtime_completions() noexcept -> void {
    join_base_t::drain_completions(
        [this](node_frame &frame) noexcept { release_frame(frame); },
        [this](node_frame &&frame, wh::core::result<graph_value> &&executed)
            -> wh::core::result<void> {
          return settle_async_node(std::move(frame), std::move(executed));
        });
  }

  // Runs the common drive preflight once: stop polling, child completion
  // drain, interrupt boundary checks, and freeze-stage launch.
  [[nodiscard]] auto begin_drive_iteration() noexcept -> bool {
    if (this->callback_active()) {
      return false;
    }
    this->poll_outer_stop();
    drain_runtime_completions();
    if (!this->finished() && this->finish_status_.has_value()) {
      this->maybe_deliver_finish();
    }
    if (this->finished()) {
      return false;
    }

    auto boundary_interrupt = state_->check_external_interrupt_boundary();
    if (boundary_interrupt.has_error()) {
      this->finish(
          wh::core::result<graph_value>::failure(boundary_interrupt.error()));
      this->maybe_deliver_finish();
      return false;
    }
    if (!state_->freeze_requested()) {
      return true;
    }

    if (this->children_.active_count() == 0U) {
      auto started = launch_freeze_stage();
      if (started.has_error()) {
        this->finish(wh::core::result<graph_value>::failure(started.error()));
        this->maybe_deliver_finish();
      }
    }
    return false;
  }

  auto finish_on_quiescent_boundary() noexcept -> void {
    if (state_->interrupt_state().wait_mode_active) {
      state_->request_freeze(true);
      auto started = launch_freeze_stage();
      if (started.has_error()) {
        this->finish(wh::core::result<graph_value>::failure(started.error()));
        this->maybe_deliver_finish();
      }
      return;
    }
    this->finish(state_->finish_graph_status());
    this->maybe_deliver_finish();
  }

  std::optional<state_t> state_{};
};

} // namespace wh::compose
