#pragma once

#include "wh/compose/graph/detail/invoke_stage_run.hpp"
#include "wh/compose/graph/detail/runtime/pregel_runtime.hpp"
#include "wh/compose/graph/graph.hpp"

namespace wh::compose {

inline auto
detail::invoke_runtime::pregel_runtime::make_input_sender(
    const attempt_id attempt)
    -> graph_sender {
  auto &invoke = session_.invoke_state();
  auto &attempt_slot = session_.slot(attempt);
  return session_.owner_->build_pregel_node_input_sender(
      attempt_slot.node_id, pregel_delivery_.current[attempt_slot.node_id],
      session_.io_storage_, session_.context_, std::addressof(attempt_slot),
      invoke.config,
      *invoke.work_scheduler);
}

inline auto detail::invoke_runtime::pregel_runtime::make_input_attempt(
    const std::uint32_t node_id, const std::size_t step)
    -> wh::core::result<attempt_id> {
  return session_.make_input_attempt(node_id, step);
}

inline auto detail::invoke_runtime::pregel_runtime::begin_state_pre(
    const attempt_id attempt) -> wh::core::result<state_step> {
  return session_.begin_state_pre(attempt);
}

inline auto detail::invoke_runtime::pregel_runtime::commit_terminal_input(
    const attempt_id attempt, graph_value input) -> wh::core::result<void> {
  auto &attempt_slot = session_.slot(attempt);
  auto stored = session_.store_output(attempt_slot.node_id, std::move(input));
  if (stored.has_error()) {
    try_persist_checkpoint();
    return wh::core::result<void>::failure(stored.error());
  }
  session_.state_table_.update(attempt_slot.node_id,
                               graph_node_lifecycle_state::completed, 0U,
                               std::nullopt);
  session_.append_transition(attempt_slot.node_id,
                             graph_state_transition_event{
                                 .kind = graph_state_transition_kind::route_commit,
                                 .cause = attempt_slot.cause,
                                 .lifecycle =
                                     graph_node_lifecycle_state::completed,
                             });
  session_.release_attempt(attempt);
  return {};
}

inline auto detail::invoke_runtime::pregel_runtime::finish()
    -> wh::core::result<graph_value> {
  auto &invoke = session_.invoke_state();
  const auto final_node_id = session_.end_id();
  const auto &final_node_key = session_.node_key(final_node_id);
  auto final_state = session_.state_table_.by_id(final_node_id);
  if (final_state.has_error() ||
      final_state.value().lifecycle != graph_node_lifecycle_state::completed) {
    session_.owner_->publish_graph_run_error(
        invoke.outputs, session_.runtime_node_path(final_node_id),
        final_node_key,
        compose_error_phase::execute, wh::core::errc::contract_violation,
        "end node was not executed");
    invoke.outputs.completed_node_keys = session_.completed_node_keys();
    try_persist_checkpoint();
    return wh::core::result<graph_value>::failure(
        wh::core::errc::contract_violation);
  }
  if (!session_.output_valid().test(final_node_id)) {
    session_.owner_->publish_graph_run_error(
        invoke.outputs, session_.runtime_node_path(final_node_id),
        final_node_key,
        compose_error_phase::execute, wh::core::errc::not_found,
        "end node output not found");
    try_persist_checkpoint();
    return wh::core::result<graph_value>::failure(wh::core::errc::not_found);
  }
  try_persist_checkpoint();
  auto final_output =
      session_.owner_->take_node_output(final_node_id, session_.io_storage_);
  if (final_output.has_value()) {
    session_.output_valid().clear(final_node_id);
  }
  return final_output;
}

template <typename receiver_t, typename graph_scheduler_t>
class detail::invoke_runtime::pregel_run
    : public detail::invoke_runtime::invoke_stage_run<
          detail::invoke_runtime::pregel_runtime, receiver_t,
          detail::invoke_runtime::pregel_run<receiver_t, graph_scheduler_t>,
          graph_scheduler_t> {
  using base_t =
      invoke_stage_run<detail::invoke_runtime::pregel_runtime, receiver_t,
                       pregel_run<receiver_t, graph_scheduler_t>,
                       graph_scheduler_t>;
  friend base_t;

public:
  template <typename graph_scheduler_u>
  pregel_run(detail::invoke_runtime::pregel_runtime &&state,
             receiver_t &&receiver, graph_scheduler_u &&graph_scheduler)
      : base_t(std::move(state), std::move(receiver),
               std::forward<graph_scheduler_u>(graph_scheduler)) {}

  auto enqueue_committed_node(const std::uint32_t) -> void {}

  [[nodiscard]] auto build_input_sender(const attempt_id attempt)
      -> graph_sender {
    return this->state().make_input_sender(attempt);
  }

  [[nodiscard]] auto build_freeze_sender(const bool external_interrupt)
      -> graph_sender {
    return this->state().make_freeze_sender(
        this->state().capture_pending_inputs(), external_interrupt);
  }

  template <typename enqueue_fn_t>
  auto commit_node_output(const attempt_id attempt, graph_value node_output,
                          enqueue_fn_t &&enqueue_fn) -> wh::core::result<void> {
    return this->state().commit_node_output(
        attempt, std::move(node_output),
        std::forward<enqueue_fn_t>(enqueue_fn));
  }

  auto prepare_superstep(const bool advance_step) -> wh::core::result<void> {
    auto &session = this->session();
    auto &invoke = session.invoke_state();
    const auto &frontier = this->state().pregel_delivery().current_frontier();
    if (frontier.empty()) {
      return {};
    }
    if (advance_step) {
      ++invoke.step_count;
    }
    const auto step = invoke.step_count;
    if (advance_step && step > invoke.step_budget) {
      const auto node_id = frontier.front();
      const auto &node_key = session.node_key(node_id);
      auto completed_node_keys = session.completed_node_keys();
      invoke.outputs.completed_node_keys = completed_node_keys;
      invoke.outputs.step_limit_error =
          graph_step_limit_error_detail{
              .step = step,
              .budget = invoke.step_budget,
              .node = node_key,
              .completed_node_keys = std::move(completed_node_keys),
          };
      session.publish_graph_error(session.runtime_node_path(node_id), node_key,
                                  compose_error_phase::schedule,
                                  wh::core::errc::timeout,
                                  "step budget exceeded");
      this->state().try_persist_checkpoint();
      return wh::core::result<void>::failure(wh::core::errc::timeout);
    }

    prepared_actions_.clear();
    prepared_actions_.reserve(frontier.size());
    for (const auto node_id : frontier) {
      prepared_actions_.push_back(
          this->state().take_ready_action(node_id, step));
    }
    prepared_head_ = 0U;
    this->state().set_superstep_active(true);
    return {};
  }

  auto start_prepared_action(pregel_action action) -> wh::core::result<void> {
    switch (action.action) {
    case pregel_action::kind::waiting:
      return {};
    case pregel_action::kind::skip: {
      auto skipped = this->state().commit_skip_action(action);
      if (skipped.has_error()) {
        return wh::core::result<void>::failure(skipped.error());
      }
      return {};
    }
    case pregel_action::kind::terminal_error:
      return wh::core::result<void>::failure(action.error);
    case pregel_action::kind::launch:
      return this->launch_input_stage(action.attempt);
    }
    return {};
  }

  auto resume() noexcept -> void {
    while (true) {
      if (!this->begin_resume_iteration()) {
        break;
      }

      if (this->state().superstep_active() && prepared_actions_.empty()) {
        auto prepared = prepare_superstep(false);
        if (prepared.has_error()) {
          this->finish(
              wh::core::result<graph_value>::failure(prepared.error()));
          break;
        }
      }

      const auto &frontier = this->state().pregel_delivery().current_frontier();
      if (!this->state().superstep_active()) {
        if (frontier.empty()) {
          this->finish_on_quiescent_boundary();
          break;
        }
        auto begun = prepare_superstep(true);
        if (begun.has_error()) {
          this->finish(wh::core::result<graph_value>::failure(begun.error()));
          break;
        }
      }

      while (!this->terminal_pending() &&
             prepared_head_ < prepared_actions_.size() &&
             this->active_child_count() <
                 this->session().max_parallel_nodes()) {
        auto started = start_prepared_action(
            std::move(prepared_actions_[prepared_head_++]));
        if (started.has_error()) {
          this->finish(wh::core::result<graph_value>::failure(started.error()));
          break;
        }
      }

      if (this->terminal_pending()) {
        break;
      }

      if (this->completion_ready()) {
        continue;
      }

      if (prepared_head_ >= prepared_actions_.size() &&
          this->active_child_count() == 0U) {
        this->state().pregel_delivery().advance_superstep();
        prepared_actions_.clear();
        prepared_head_ = 0U;
        this->state().set_superstep_active(false);
        continue;
      }

      break;
    }
  }

private:
  std::vector<pregel_action> prepared_actions_{};
  std::size_t prepared_head_{0U};
};

} // namespace wh::compose
