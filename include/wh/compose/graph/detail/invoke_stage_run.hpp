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
            state.session().node_count() + 1U,
            graph_scheduler_t{std::forward<graph_scheduler_u>(graph_scheduler)},
            std::forward<receiver_arg_t>(receiver)) {
    state_.emplace(std::move(state));
    state_->rebind_moved_runtime_storage();
  }

  auto bind_outer_stop() noexcept -> void { join_base_t::bind_outer_stop(); }

  auto request_resume() noexcept -> void { join_base_t::request_resume(); }

protected:
  [[nodiscard]] auto state() noexcept -> state_t & { return *state_; }

  [[nodiscard]] auto state() const noexcept -> const state_t & {
    return *state_;
  }

  [[nodiscard]] auto session() noexcept -> invoke_session & {
    return state_->session();
  }

  [[nodiscard]] auto session() const noexcept -> const invoke_session & {
    return state_->session();
  }

public:
  auto start() noexcept -> void { join_base_t::start(); }

  auto prepare_finish_delivery() noexcept -> void {
    if (!state_.has_value()) {
      return;
    }
    session().publish_runtime_outputs();
    state_.reset();
  }

protected:
  auto release_frame(node_frame &frame) noexcept -> void {
    frame.node_local_scope.release(session().node_local_process_states_);
  }

  auto launch_input_stage(node_frame &&frame) -> wh::core::result<void> {
    auto sender =
        static_cast<derived_t &>(*this).build_input_sender(std::addressof(frame));
    auto started = this->start_child(std::move(sender), std::move(frame));
    return started;
  }

  auto launch_state_stage(node_frame &&frame, graph_sender sender)
      -> wh::core::result<void> {
    return this->start_child(std::move(sender), std::move(frame));
  }

  auto launch_freeze_stage() -> wh::core::result<void> {
    node_frame frame{};
    frame.stage = stage::freeze;
    frame.node_id = session().control_slot_id();
    return this->start_child(
        static_cast<derived_t &>(*this).build_freeze_sender(
            session().freeze_external()),
        std::move(frame));
  }

  auto launch_node_stage(node_frame &&frame) -> wh::core::result<void> {
    if (frame.node == nullptr) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    if (frame.node_input.has_value()) {
      if (compiled_node_is_sync(*frame.node)) {
        auto executed = invoke_session::run_sync_node_execution(
            *frame.node, *frame.node_input, session().context_,
            session().invoke_state().bound_call_scope, std::addressof(session()),
            frame,
            [this](const std::uint32_t node_id,
                   const std::size_t step) noexcept {
              session().emit_debug(
                  graph_debug_stream_event::decision_kind::retry, node_id,
                  step);
            });
        return settle_node_stage(std::move(frame), std::move(executed));
      }
      return this->start_child(invoke_session::make_async_node_attempt_sender(
                                   *frame.node, *frame.node_input,
                                   session().context_,
                                   session().invoke_state().bound_call_scope,
                                   std::addressof(session()), frame),
                               std::move(frame));
    }

    if (!session().should_retain_input(frame)) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }

    auto *pending_input = session().pending_inputs_.find_input(frame.node_id);
    if (pending_input == nullptr) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    auto execution_input =
        frame.node->meta.input_contract == node_contract::stream
            ? detail::fork_graph_reader_payload(*pending_input)
            : fork_graph_value(*pending_input);
    if (execution_input.has_error()) {
      return wh::core::result<void>::failure(execution_input.error());
    }
    if (compiled_node_is_sync(*frame.node)) {
      auto live_input = std::move(execution_input).value();
      auto executed = invoke_session::run_sync_node_execution(
          *frame.node, live_input, session().context_,
          session().invoke_state().bound_call_scope, std::addressof(session()),
          frame,
          [this](const std::uint32_t node_id, const std::size_t step) noexcept {
            session().emit_debug(
                graph_debug_stream_event::decision_kind::retry, node_id, step);
          });
      return settle_node_stage(std::move(frame), std::move(executed));
    }
    frame.node_input.emplace(std::move(execution_input).value());
    return this->start_child(invoke_session::make_async_node_attempt_sender(
                                 *frame.node, *frame.node_input,
                                 session().context_,
                                 session().invoke_state().bound_call_scope,
                                 std::addressof(session()), frame),
                             std::move(frame));
  }

  auto continue_node_stage(node_frame &&frame, graph_value input)
      -> wh::core::result<void> {
    auto prepared_input =
        session().prepare_execution_input(std::move(frame), std::move(input));
    if (prepared_input.has_error()) {
      state_->try_persist_checkpoint();
      return wh::core::result<void>::failure(prepared_input.error());
    }
    auto prepared_stage = std::move(prepared_input).value();
    if (prepared_stage.sender.has_value()) {
      return launch_state_stage(std::move(prepared_stage.frame),
                                std::move(*prepared_stage.sender));
    }

    auto next = session().finalize_node_frame(std::move(prepared_stage.frame),
                                              std::move(prepared_stage.payload));
    if (next.has_error()) {
      state_->try_persist_checkpoint();
      return wh::core::result<void>::failure(next.error());
    }

    auto prepared = std::move(next).value();
    return launch_node_stage(std::move(prepared));
  }

  auto settle_pre_state_stage(node_frame &&frame,
                              wh::core::result<graph_value> &&resolved)
      -> wh::core::result<void> {
    if (resolved.has_error()) {
      auto failed = session().fail_node_stage(std::move(frame), resolved.error(),
                                              "node pre-state handler failed");
      state_->try_persist_checkpoint();
      return failed;
    }
    return continue_node_stage(std::move(frame), std::move(resolved).value());
  }

  auto settle_prepare_stage(node_frame &&frame,
                            wh::core::result<graph_value> &&resolved)
      -> wh::core::result<void> {
    if (resolved.has_error()) {
      auto failed = session().fail_node_stage(
          std::move(frame), resolved.error(),
          "node execution input normalization failed");
      state_->try_persist_checkpoint();
      return failed;
    }
    return continue_node_stage(std::move(frame), std::move(resolved).value());
  }

  auto settle_post_state_stage(node_frame &&frame,
                               wh::core::result<graph_value> &&resolved)
      -> wh::core::result<void> {
    if (resolved.has_error()) {
      auto failed = session().fail_node_stage(std::move(frame), resolved.error(),
                                              "node post-state handler failed");
      state_->try_persist_checkpoint();
      return failed;
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
      if (!session().freeze_requested() && frame.attempt < frame.retry_budget) {
        if (session().should_retain_input(frame)) {
          frame.node_input.reset();
        }
        session().emit_debug(graph_debug_stream_event::decision_kind::retry,
                             frame.node_id, frame.cause.step);
        ++frame.attempt;
        return launch_node_stage(std::move(frame));
      }

      const auto error_code = executed.error();
      if (session().should_wrap_node_error(error_code)) {
        auto failed = session().fail_node_stage(std::move(frame), error_code,
                                                "node execution failed");
        state_->try_persist_checkpoint();
        return failed;
      }
      session().state_table_.update(frame.node_id,
                                    graph_node_lifecycle_state::canceled,
                                    frame.retry_budget + 1U, error_code);
      session().append_transition(
          frame.node_id, graph_state_transition_event{
                             .kind = graph_state_transition_kind::node_leave,
                             .cause = frame.cause,
                             .lifecycle = graph_node_lifecycle_state::canceled,
                         });
      state_->try_persist_checkpoint();
      frame.node_local_scope.release(session().node_local_process_states_);
      return wh::core::result<void>::failure(error_code);
    }

    auto post =
        session().begin_state_post(std::move(frame), std::move(executed).value());
    if (post.has_error()) {
      state_->try_persist_checkpoint();
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
      if (resolved.error() == wh::core::errc::not_found ||
          resolved.error() == wh::core::errc::contract_violation) {
        auto *pending_input = session().pending_inputs_.find_input(frame.node_id);
        if (pending_input != nullptr &&
            session().pending_inputs_.restored_input(frame.node_id)) {
          auto restored_input =
              frame.node != nullptr &&
                      frame.node->meta.input_contract == node_contract::stream
                  ? detail::fork_graph_reader_payload(*pending_input)
                  : fork_graph_value(*pending_input);
          if (restored_input.has_value()) {
            resolved = std::move(restored_input);
          } else {
            resolved = std::move(restored_input);
          }
        }
      }
    }
    if (resolved.has_error()) {
      session().publish_node_error(session().runtime_node_path(frame.node_id),
                                 frame.node_id, resolved.error(),
                                 "node input resolution failed");
      session().state_table_.update(frame.node_id,
                                    graph_node_lifecycle_state::failed, 1U,
                                    resolved.error());
      session().append_transition(
          frame.node_id, graph_state_transition_event{
                             .kind = graph_state_transition_kind::node_fail,
                             .cause = frame.cause,
                             .lifecycle = graph_node_lifecycle_state::failed,
                         });
      state_->try_persist_checkpoint();
      return wh::core::result<void>::failure(resolved.error());
    }
    auto input = std::move(resolved).value();

    if (frame.node_id == session().end_id()) {
      const auto node_id = frame.node_id;
      auto committed =
          state_->commit_terminal_input(std::move(frame), std::move(input));
      if (committed.has_error()) {
        return committed;
      }
      static_cast<derived_t &>(*this).enqueue_committed_node(node_id);
      return {};
    }

    auto prepared = state_->begin_state_pre(std::move(frame), std::move(input));
    if (prepared.has_error()) {
      if (prepared.error() == wh::core::errc::canceled &&
          session().freeze_requested()) {
        return {};
      }
      state_->try_persist_checkpoint();
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

  // Runs the common resume preflight once: stop polling, child completion
  // drain, and then either enters terminal delivery or continues with
  // boundary/freeze scheduling while the runtime is still open.
  [[nodiscard]] auto begin_resume_iteration() noexcept -> bool {
    drain_runtime_completions();
    if (this->finished() || this->terminal_pending()) {
      return false;
    }

    if (this->stop_requested()) {
      this->finish(
          wh::core::result<graph_value>::failure(wh::core::errc::canceled));
      return false;
    }

    auto boundary_interrupt = session().check_external_interrupt_boundary();
    if (boundary_interrupt.has_error()) {
      this->finish(
          wh::core::result<graph_value>::failure(boundary_interrupt.error()));
      return false;
    }
    if (!session().freeze_requested()) {
      return true;
    }

    if (this->active_child_count() == 0U) {
      auto started = launch_freeze_stage();
      if (started.has_error()) {
        this->finish(wh::core::result<graph_value>::failure(started.error()));
      }
    }
    return false;
  }

  auto finish_on_quiescent_boundary() noexcept -> void {
    if (this->finished() || this->terminal_pending()) {
      return;
    }
    if (session().interrupt_state().wait_mode_active) {
      session().request_freeze(true);
      auto started = launch_freeze_stage();
      if (started.has_error()) {
        this->finish(wh::core::result<graph_value>::failure(started.error()));
      }
      return;
    }
    this->finish(state_->finish());
  }

  std::optional<state_t> state_{};
};

} // namespace wh::compose
