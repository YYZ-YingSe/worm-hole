#pragma once

#include "wh/compose/graph/graph.hpp"
#include "wh/compose/graph/detail/dag.hpp"
#include "wh/compose/graph/detail/pregel.hpp"

namespace wh::compose {

inline detail::invoke_runtime::run_state::run_state(
    const graph *owner, graph_value &&input, wh::core::run_context &context,
    graph_call_options &&call_options,
    wh::core::detail::any_resume_scheduler_t resume_scheduler,
    node_path path_prefix, graph_process_state *parent_process_state,
    detail::runtime_state::invoke_outputs *nested_outputs)
    : owner_(owner), context_(context), path_prefix_(std::move(path_prefix)),
      parent_process_state_(parent_process_state),
      nested_outputs_(nested_outputs),
      resume_scheduler_(std::move(resume_scheduler)) {
  owned_call_options_ =
      std::make_unique<graph_call_options>(std::move(call_options));
  initialize(std::move(input), graph_call_scope{*owned_call_options_});
}

inline detail::invoke_runtime::run_state::run_state(
    const graph *owner, graph_value &&input, wh::core::run_context &context,
    graph_call_scope call_scope,
    wh::core::detail::any_resume_scheduler_t resume_scheduler,
    node_path path_prefix, graph_process_state *parent_process_state,
    detail::runtime_state::invoke_outputs *nested_outputs)
    : owner_(owner), context_(context), path_prefix_(std::move(path_prefix)),
      parent_process_state_(parent_process_state),
      nested_outputs_(nested_outputs),
      resume_scheduler_(std::move(resume_scheduler)) {
  initialize(std::move(input), std::move(call_scope));
}

template <typename receiver_t, typename lane_scheduler_t>
inline auto detail::invoke_runtime::run_state::launch(
    run_state &&state, receiver_t receiver,
    lane_scheduler_t lane_scheduler) noexcept -> void {
  using stored_receiver_t = std::remove_cvref_t<receiver_t>;
  using stored_lane_scheduler_t = std::remove_cvref_t<lane_scheduler_t>;
  try {
    if (state.init_error_.has_value()) {
      state.publish_runtime_outputs();
      stdexec::set_value(std::move(receiver),
                         wh::core::result<graph_value>::failure(
                             *state.init_error_));
      return;
    }
    if (state.owner_->options_.mode == graph_runtime_mode::dag) {
      auto shared =
          std::make_shared<dag_run<stored_receiver_t, stored_lane_scheduler_t>>(
              std::move(state), std::move(receiver), std::move(lane_scheduler));
      shared->bind_outer_stop(shared);
      shared->request_pump(shared);
      return;
    }
    auto shared =
        std::make_shared<pregel_run<stored_receiver_t, stored_lane_scheduler_t>>(
            std::move(state), std::move(receiver), std::move(lane_scheduler));
    shared->bind_outer_stop(shared);
    shared->request_pump(shared);
  } catch (...) {
    stdexec::set_value(std::move(receiver),
                       wh::core::result<graph_value>::failure(
                           wh::core::map_current_exception()));
  }
}

inline auto detail::invoke_runtime::run_state::start(
    run_state &&state) -> graph_sender {
  if (state.init_error_.has_value()) {
    return state.immediate_failure(*state.init_error_);
  }
  if (state.owner_->options_.mode == graph_runtime_mode::dag) {
    return std::move(state).start_dag();
  }
  return std::move(state).start_pregel();
}

inline auto detail::invoke_runtime::run_state::start_dag() && -> graph_sender {
  return graph_sender{exec::create<
      stdexec::completion_signatures<
          stdexec::set_value_t(wh::core::result<graph_value>)>>(
      [state = std::move(*this)](auto &create_context) mutable noexcept {
        auto lane_scheduler =
            wh::core::detail::any_resume_scheduler_t{*state.resume_scheduler_};
        launch(std::move(state), std::move(create_context.receiver),
                       std::move(lane_scheduler));
      })};
}

inline auto detail::invoke_runtime::run_state::start_pregel() && -> graph_sender {
  return graph_sender{exec::create<
      stdexec::completion_signatures<
          stdexec::set_value_t(wh::core::result<graph_value>)>>(
      [state = std::move(*this)](auto &create_context) mutable noexcept {
        auto lane_scheduler =
            wh::core::detail::any_resume_scheduler_t{*state.resume_scheduler_};
        launch(std::move(state), std::move(create_context.receiver),
                       std::move(lane_scheduler));
      })};
}

} // namespace wh::compose
