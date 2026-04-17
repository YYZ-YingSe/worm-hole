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
    session().release_attempts();
    session().publish_runtime_outputs();
    state_.reset();
  }

protected:
  auto release_attempt(const attempt_id attempt) noexcept -> void {
    session().release_attempt(attempt);
  }

  auto launch_input_stage(const attempt_id attempt) -> wh::core::result<void> {
    auto sender = static_cast<derived_t &>(*this).build_input_sender(attempt);
    return this->start_child(std::move(sender), attempt);
  }

  auto launch_state_stage(const attempt_id attempt, graph_sender sender)
      -> wh::core::result<void> {
    return this->start_child(std::move(sender), attempt);
  }

  auto launch_freeze_stage() -> wh::core::result<void> {
    const auto attempt = session().control_attempt_id();
    session().release_attempt(attempt);
    auto &attempt_slot = session().slot(attempt);
    attempt_slot.stage = stage::freeze;
    attempt_slot.node_id = attempt.slot;
    return this->start_child(
        static_cast<derived_t &>(*this).build_freeze_sender(
            session().freeze_external()),
        attempt);
  }

  auto restore_attempt_input(const attempt_id attempt)
      -> wh::core::result<void> {
    auto &attempt_slot = session().slot(attempt);
    if (attempt_slot.node == nullptr) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    if (!session().should_retain_input(attempt_slot)) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    auto *pending_input =
        session().pending_inputs_.find_input(attempt_slot.node_id);
    if (pending_input == nullptr) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    auto execution_input =
        attempt_slot.node->meta.input_contract == node_contract::stream
            ? detail::fork_graph_reader_payload(*pending_input)
            : fork_graph_value(*pending_input);
    if (execution_input.has_error()) {
      return wh::core::result<void>::failure(execution_input.error());
    }
    if (!attempt_slot.input.has_value()) {
      attempt_slot.input.emplace();
    }
    attempt_slot.input->payload.emplace(std::move(execution_input).value());
    return {};
  }

  auto launch_node_with_live_input(const attempt_id attempt)
      -> wh::core::result<void> {
    auto &attempt_slot = session().slot(attempt);
    if (attempt_slot.node == nullptr) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    if (!attempt_slot.input.has_value() ||
        !attempt_slot.input->payload.has_value()) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    auto &live_input = *attempt_slot.input->payload;
    const auto dispatch =
        session().resolve_node_sync_dispatch(attempt_slot.node_id);
    if (compiled_node_is_sync(*attempt_slot.node)) {
      if (dispatch == sync_dispatch::work) {
        return this->start_child(invoke_session::make_sync_node_attempt_sender(
                                     *attempt_slot.node, live_input,
                                     session().context_,
                                     session().invoke_state().bound_call_scope,
                                     std::addressof(session()), attempt_slot),
                                 attempt);
      }
      auto executed = invoke_session::run_sync_node_execution(
          *attempt_slot.node, live_input, session().context_,
          session().invoke_state().bound_call_scope, std::addressof(session()),
          attempt_slot);
      return settle_node(attempt, std::move(executed));
    }
    return this->start_child(invoke_session::make_async_node_attempt_sender(
                                 *attempt_slot.node, live_input,
                                 session().context_,
                                 session().invoke_state().bound_call_scope,
                                 std::addressof(session()), attempt_slot),
                             attempt);
  }

  auto launch_node_from_retained_input(const attempt_id attempt)
      -> wh::core::result<void> {
    auto restored = restore_attempt_input(attempt);
    if (restored.has_error()) {
      return restored;
    }
    return launch_node_with_live_input(attempt);
  }

  auto launch_node_stage(const attempt_id attempt) -> wh::core::result<void> {
    auto &attempt_slot = session().slot(attempt);
    const bool has_live_input =
        attempt_slot.input.has_value() && attempt_slot.input->payload.has_value();
    if (has_live_input) {
      return launch_node_with_live_input(attempt);
    }
    return launch_node_from_retained_input(attempt);
  }

  auto continue_attempt(const attempt_id attempt) -> wh::core::result<void> {
    auto prepared_input = session().prepare_execution_input(attempt);
    if (prepared_input.has_error()) {
      state_->try_persist_checkpoint();
      session().release_attempt(attempt);
      return wh::core::result<void>::failure(prepared_input.error());
    }
    auto prepared_stage = std::move(prepared_input).value();
    if (prepared_stage.sender.has_value()) {
      return launch_state_stage(prepared_stage.attempt,
                                std::move(*prepared_stage.sender));
    }

    auto finalized = session().finalize_node_attempt(prepared_stage.attempt);
    if (finalized.has_error()) {
      state_->try_persist_checkpoint();
      return wh::core::result<void>::failure(finalized.error());
    }

    return launch_node_with_live_input(prepared_stage.attempt);
  }

  auto settle_pre_state(const attempt_id attempt,
                        wh::core::result<graph_value> &&resolved)
      -> wh::core::result<void> {
    if (resolved.has_error()) {
      auto failed = session().fail_node_stage(attempt, resolved.error(),
                                              "node pre-state handler failed");
      state_->try_persist_checkpoint();
      return failed;
    }
    auto stored =
        session().store_attempt_input(attempt, std::move(resolved).value());
    if (stored.has_error()) {
      state_->try_persist_checkpoint();
      session().release_attempt(attempt);
      return wh::core::result<void>::failure(stored.error());
    }
    return continue_attempt(attempt);
  }

  auto settle_prepare(const attempt_id attempt,
                      wh::core::result<graph_value> &&resolved)
      -> wh::core::result<void> {
    if (resolved.has_error()) {
      auto failed = session().fail_node_stage(
          attempt, resolved.error(),
          "node execution input normalization failed");
      state_->try_persist_checkpoint();
      return failed;
    }
    auto stored =
        session().store_attempt_input(attempt, std::move(resolved).value());
    if (stored.has_error()) {
      state_->try_persist_checkpoint();
      session().release_attempt(attempt);
      return wh::core::result<void>::failure(stored.error());
    }
    return continue_attempt(attempt);
  }

  auto settle_post_state(const attempt_id attempt,
                         wh::core::result<graph_value> &&resolved)
      -> wh::core::result<void> {
    if (resolved.has_error()) {
      auto failed = session().fail_node_stage(attempt, resolved.error(),
                                              "node post-state handler failed");
      state_->try_persist_checkpoint();
      return failed;
    }
    return static_cast<derived_t &>(*this).commit_node_output(
        attempt, std::move(resolved).value(),
        [this](const std::uint32_t node_id) {
          static_cast<derived_t &>(*this).enqueue_committed_node(node_id);
        });
  }

  auto settle_node(const attempt_id attempt,
                   wh::core::result<graph_value> &&executed)
      -> wh::core::result<void> {
    auto &attempt_slot = session().slot(attempt);
    if (executed.has_error()) {
      if (!session().freeze_requested() &&
          attempt_slot.attempt < attempt_slot.retry_budget) {
        if (session().should_retain_input(attempt_slot) &&
            attempt_slot.input.has_value()) {
          attempt_slot.input->payload.reset();
        }
        session().emit_debug(graph_debug_stream_event::decision_kind::retry,
                             attempt_slot.node_id, attempt_slot.cause.step);
        ++attempt_slot.attempt;
        return launch_node_stage(attempt);
      }

      const auto error_code = executed.error();
      if (session().should_wrap_node_error(error_code)) {
        auto failed = session().fail_node_stage(attempt, error_code,
                                                "node execution failed");
        state_->try_persist_checkpoint();
        return failed;
      }
      session().state_table_.update(attempt_slot.node_id,
                                    graph_node_lifecycle_state::canceled,
                                    attempt_slot.retry_budget + 1U, error_code);
      session().append_transition(
          attempt_slot.node_id, graph_state_transition_event{
                                    .kind = graph_state_transition_kind::node_leave,
                                    .cause = attempt_slot.cause,
                                    .lifecycle =
                                        graph_node_lifecycle_state::canceled,
                                });
      state_->try_persist_checkpoint();
      session().release_attempt(attempt);
      return wh::core::result<void>::failure(error_code);
    }

    auto output = std::move(executed).value();
    auto post = session().begin_state_post(attempt, output);
    if (post.has_error()) {
      state_->try_persist_checkpoint();
      return wh::core::result<void>::failure(post.error());
    }
    auto post_sender = std::move(post).value();
    if (post_sender.has_value()) {
      return launch_state_stage(attempt, std::move(*post_sender));
    }
    return static_cast<derived_t &>(*this).commit_node_output(
        attempt, std::move(output),
        [this](const std::uint32_t node_id) {
          static_cast<derived_t &>(*this).enqueue_committed_node(node_id);
        });
  }

  auto settle_input(const attempt_id attempt,
                    wh::core::result<graph_value> &&resolved)
      -> wh::core::result<void> {
    auto &attempt_slot = session().slot(attempt);
    if (resolved.has_error()) {
      if (resolved.error() == wh::core::errc::not_found ||
          resolved.error() == wh::core::errc::contract_violation) {
        auto *pending_input =
            session().pending_inputs_.find_input(attempt_slot.node_id);
        if (pending_input != nullptr &&
            session().pending_inputs_.restored_input(attempt_slot.node_id)) {
          auto restored_input =
              attempt_slot.node != nullptr &&
                      attempt_slot.node->meta.input_contract == node_contract::stream
                  ? detail::fork_graph_reader_payload(*pending_input)
                  : fork_graph_value(*pending_input);
          resolved = std::move(restored_input);
        }
      }
    }
    if (resolved.has_error()) {
      session().publish_node_error(
          session().runtime_node_path(attempt_slot.node_id),
          attempt_slot.node_id, resolved.error(),
          "node input resolution failed");
      session().state_table_.update(attempt_slot.node_id,
                                    graph_node_lifecycle_state::failed, 1U,
                                    resolved.error());
      session().append_transition(
          attempt_slot.node_id, graph_state_transition_event{
                                    .kind = graph_state_transition_kind::node_fail,
                                    .cause = attempt_slot.cause,
                                    .lifecycle =
                                        graph_node_lifecycle_state::failed,
                                });
      state_->try_persist_checkpoint();
      session().release_attempt(attempt);
      return wh::core::result<void>::failure(resolved.error());
    }
    auto input = std::move(resolved).value();

    if (attempt_slot.node_id == session().end_id()) {
      const auto node_id = attempt_slot.node_id;
      auto committed = state_->commit_terminal_input(attempt, std::move(input));
      if (committed.has_error()) {
        return committed;
      }
      static_cast<derived_t &>(*this).enqueue_committed_node(node_id);
      return {};
    }

    auto stored = session().store_attempt_input(attempt, std::move(input));
    if (stored.has_error()) {
      state_->try_persist_checkpoint();
      session().release_attempt(attempt);
      return wh::core::result<void>::failure(stored.error());
    }

    auto prepared = state_->begin_state_pre(attempt);
    if (prepared.has_error()) {
      if (prepared.error() == wh::core::errc::canceled &&
          session().freeze_requested()) {
        session().release_attempt(attempt);
        return {};
      }
      state_->try_persist_checkpoint();
      session().release_attempt(attempt);
      return wh::core::result<void>::failure(prepared.error());
    }

    auto prepared_stage = std::move(prepared).value();
    if (prepared_stage.sender.has_value()) {
      return launch_state_stage(prepared_stage.attempt,
                                std::move(*prepared_stage.sender));
    }
    return continue_attempt(prepared_stage.attempt);
  }

  auto settle_async(const attempt_id attempt,
                    wh::core::result<graph_value> &&executed)
      -> wh::core::result<void> {
    const auto current_stage = session().slot(attempt).stage;
    if (current_stage == stage::input) {
      return settle_input(attempt, std::move(executed));
    }
    if (current_stage == stage::pre_state) {
      return settle_pre_state(attempt, std::move(executed));
    }
    if (current_stage == stage::prepare) {
      return settle_prepare(attempt, std::move(executed));
    }
    if (current_stage == stage::post_state) {
      return settle_post_state(attempt, std::move(executed));
    }
    if (current_stage == stage::freeze) {
      session().release_attempt(attempt);
      if (executed.has_error()) {
        return wh::core::result<void>::failure(executed.error());
      }
      this->enter_terminal(
          wh::core::result<graph_value>::failure(wh::core::errc::canceled));
      return {};
    }
    wh_invariant(current_stage == stage::node);
    return settle_node(attempt, std::move(executed));
  }

  auto drain_runtime_completions() noexcept -> void {
    join_base_t::drain_completions(
        [this](const attempt_id attempt) noexcept { release_attempt(attempt); },
        [this](const attempt_id attempt, wh::core::result<graph_value> &&executed)
            -> wh::core::result<void> {
          return settle_async(attempt, std::move(executed));
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
      this->enter_terminal(
          wh::core::result<graph_value>::failure(wh::core::errc::canceled));
      return false;
    }

    auto boundary_interrupt = session().check_external_interrupt_boundary();
    if (boundary_interrupt.has_error()) {
      this->enter_terminal(
          wh::core::result<graph_value>::failure(boundary_interrupt.error()));
      return false;
    }
    if (!session().freeze_requested()) {
      return true;
    }

    if (this->active_child_count() == 0U) {
      auto started = launch_freeze_stage();
      if (started.has_error()) {
        this->enter_terminal(wh::core::result<graph_value>::failure(started.error()));
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
        this->enter_terminal(wh::core::result<graph_value>::failure(started.error()));
      }
      return;
    }
    this->enter_terminal(state_->finish());
  }

  std::optional<state_t> state_{};
};

} // namespace wh::compose
