#pragma once

#include "wh/compose/graph/detail/invoke_stage_run.hpp"
#include "wh/compose/graph/detail/runtime/dag_runtime.hpp"
#include "wh/compose/graph/graph.hpp"
#include "wh/core/compiler.hpp"

namespace wh::compose {

inline auto detail::invoke_runtime::dag_runtime::make_input_sender(const attempt_id attempt)
    -> graph_sender {
  auto &invoke = session_.invoke_state();
  auto &attempt_slot = session_.slot(attempt);
  return session_.owner_->build_node_input_sender(
      attempt_slot.node_id, session_.io_storage_, dag_node_phases(), branch_states(),
      session_.context_, std::addressof(attempt_slot), invoke.config, *invoke.work_scheduler);
}

inline auto detail::invoke_runtime::dag_runtime::make_input_attempt(const std::uint32_t node_id,
                                                                    std::size_t step)
    -> wh::core::result<attempt_id> {
  auto attempt = session_.make_input_attempt(node_id, step);
  if (attempt.has_error()) {
    return attempt;
  }
  dag_node_phases()[node_id] = invoke_session::dag_node_phase::running;
  return attempt;
}

inline auto detail::invoke_runtime::dag_runtime::begin_state_pre(const attempt_id attempt)
    -> wh::core::result<state_step> {
  const auto node_id = session_.slot(attempt).node_id;
  auto prepared = session_.begin_state_pre(attempt);
  if (prepared.has_error() && prepared.error() == wh::core::errc::canceled &&
      session_.freeze_requested()) {
    dag_node_phases()[node_id] = invoke_session::dag_node_phase::pending;
  }
  return prepared;
}

inline auto detail::invoke_runtime::dag_runtime::commit_terminal_input(const attempt_id attempt,
                                                                       graph_value input)
    -> wh::core::result<void> {
  auto &attempt_slot = session_.slot(attempt);
  auto stored = session_.store_output(attempt_slot.node_id, std::move(input));
  if (stored.has_error()) {
    try_persist_checkpoint();
    return wh::core::result<void>::failure(stored.error());
  }
  dag_node_phases()[attempt_slot.node_id] = invoke_session::dag_node_phase::executed;
  session_.state_table_.update(attempt_slot.node_id, graph_node_lifecycle_state::completed, 0U,
                               std::nullopt);
  session_.append_transition(attempt_slot.node_id,
                             graph_state_transition_event{
                                 .kind = graph_state_transition_kind::route_commit,
                                 .cause = attempt_slot.cause,
                                 .lifecycle = graph_node_lifecycle_state::completed,
                             });
  session_.release_attempt(attempt);
  return {};
}

inline auto detail::invoke_runtime::dag_runtime::finish() -> wh::core::result<graph_value> {
  auto &invoke = session_.invoke_state();
  const auto final_node_id = session_.end_id();
  const auto &final_node_key = session_.node_key(final_node_id);
  if (dag_node_phases()[final_node_id] != invoke_session::dag_node_phase::executed) {
    session_.owner_->publish_graph_run_error(
        invoke.outputs, session_.runtime_node_path(final_node_id), final_node_key,
        compose_error_phase::execute, wh::core::errc::contract_violation,
        "end node was not executed");
    invoke.outputs.completed_node_keys = session_.completed_node_keys();
    return wh::core::result<graph_value>::failure(wh::core::errc::contract_violation);
  }
  if (!session_.output_valid().test(final_node_id)) {
    session_.owner_->publish_graph_run_error(
        invoke.outputs, session_.runtime_node_path(final_node_id), final_node_key,
        compose_error_phase::execute, wh::core::errc::not_found, "end node output not found");
    return wh::core::result<graph_value>::failure(wh::core::errc::not_found);
  }
  auto final_output = session_.owner_->take_node_output(final_node_id, session_.io_storage_);
  if (final_output.has_value()) {
    session_.output_valid().clear(final_node_id);
  }
  return final_output;
}

template <typename receiver_t, typename graph_scheduler_t>
class detail::invoke_runtime::dag_run
    : public detail::invoke_runtime::invoke_stage_run<
          detail::invoke_runtime::dag_runtime, receiver_t,
          detail::invoke_runtime::dag_run<receiver_t, graph_scheduler_t>, graph_scheduler_t> {
  using base_t = invoke_stage_run<detail::invoke_runtime::dag_runtime, receiver_t,
                                  dag_run<receiver_t, graph_scheduler_t>, graph_scheduler_t>;
  friend base_t;

public:
  template <typename graph_scheduler_u>
  dag_run(detail::invoke_runtime::dag_runtime &&state, receiver_t &&receiver,
          graph_scheduler_u &&graph_scheduler)
      : base_t(std::move(state), std::move(receiver),
               std::forward<graph_scheduler_u>(graph_scheduler)) {}

  auto enqueue_committed_node(const std::uint32_t node_id) -> void {
    this->state().enqueue_dependents(node_id);
  }

  [[nodiscard]] auto build_input_sender(const attempt_id attempt) -> graph_sender {
    return this->state().make_input_sender(attempt);
  }

  [[nodiscard]] auto build_freeze_sender(const bool external_interrupt) -> graph_sender {
    return this->state().make_freeze_sender(this->state().capture_pending_inputs(),
                                            external_interrupt);
  }

  template <typename enqueue_fn_t>
  auto commit_node_output(const attempt_id attempt, graph_value node_output,
                          enqueue_fn_t &&enqueue_fn) -> wh::core::result<void> {
    return this->state().commit_node_output(attempt, std::move(node_output),
                                            std::forward<enqueue_fn_t>(enqueue_fn));
  }

  auto resume() noexcept -> void {
    while (true) {
      if (!this->begin_resume_iteration()) {
        break;
      }

      if (this->state().dag_node_phases()[this->session().end_id()] ==
          invoke_session::dag_node_phase::executed) {
        this->finish_on_quiescent_boundary();
        break;
      }

      bool no_ready = false;
      while (!this->terminal_pending() &&
             this->active_child_count() < this->session().max_parallel_nodes()) {
        auto action = this->state().take_ready_action();
        if (action.kind == ready_action_kind::no_ready) {
          no_ready = true;
          break;
        }
        if (action.kind == ready_action_kind::continue_scan) {
          continue;
        }
        if (action.kind == ready_action_kind::terminal_error) {
          this->request_terminal_status(wh::core::result<graph_value>::failure(action.error));
          break;
        }
        auto started = this->launch_input_stage(action.attempt);
        if (started.has_error()) {
          this->request_terminal_status(wh::core::result<graph_value>::failure(started.error()));
          break;
        }
      }

      if (this->terminal_pending()) {
        break;
      }

      if (this->state().dag_node_phases()[this->session().end_id()] ==
          invoke_session::dag_node_phase::executed) {
        this->finish_on_quiescent_boundary();
        break;
      }

      if (no_ready && this->active_child_count() == 0U) {
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
