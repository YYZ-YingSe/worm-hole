#pragma once

#include "wh/compose/graph/detail/dag_start.hpp"
#include "wh/compose/graph/detail/pregel_start.hpp"
#include "wh/compose/graph/detail/runtime/dag_run_state.hpp"
#include "wh/compose/graph/detail/runtime/pregel_run_state.hpp"
#include "wh/compose/graph/graph.hpp"

namespace wh::compose {

inline detail::invoke_runtime::run_state::run_state(
    const graph *owner, graph_value &&input, wh::core::run_context &context,
    graph_call_options &&call_options,
    wh::core::detail::any_resume_scheduler_t graph_scheduler,
    node_path path_prefix, graph_process_state *parent_process_state,
    detail::runtime_state::invoke_outputs *nested_outputs,
    const graph_runtime_services *services, graph_invoke_controls controls,
    detail::runtime_state::invoke_outputs *published_outputs,
    const run_state *parent_state)
    : owner_(owner), context_(context) {
  invoke_.controls = std::move(controls);
  invoke_.services = services;
  invoke_.parent_state = parent_state;
  invoke_.path_prefix = std::move(path_prefix);
  invoke_.parent_process_state = parent_process_state;
  invoke_.nested_outputs = nested_outputs;
  invoke_.published_outputs = published_outputs;
  invoke_.graph_scheduler.emplace(std::move(graph_scheduler));
  invoke_.owned_call_options =
      std::make_unique<graph_call_options>(std::move(call_options));
  initialize(std::move(input), graph_call_scope{*invoke_.owned_call_options});
}

inline detail::invoke_runtime::run_state::run_state(
    const graph *owner, graph_value &&input, wh::core::run_context &context,
    graph_call_scope call_scope,
    wh::core::detail::any_resume_scheduler_t graph_scheduler,
    node_path path_prefix, graph_process_state *parent_process_state,
    detail::runtime_state::invoke_outputs *nested_outputs,
    const graph_runtime_services *services, graph_invoke_controls controls,
    detail::runtime_state::invoke_outputs *published_outputs,
    const run_state *parent_state)
    : owner_(owner), context_(context) {
  invoke_.controls = std::move(controls);
  invoke_.services = services;
  invoke_.parent_state = parent_state;
  invoke_.path_prefix = std::move(path_prefix);
  invoke_.parent_process_state = parent_process_state;
  invoke_.nested_outputs = nested_outputs;
  invoke_.published_outputs = published_outputs;
  invoke_.graph_scheduler.emplace(std::move(graph_scheduler));
  initialize(std::move(input), std::move(call_scope));
}

inline auto
detail::invoke_runtime::run_state::rebind_moved_runtime_storage() noexcept
    -> void {
  if (invoke_.parent_state == nullptr) {
    invoke_.forwarded_checkpoints =
        std::addressof(invoke_.owned_forwarded_checkpoints);
  }
  if (invoke_.owned_call_options == nullptr) {
    return;
  }
  auto rebound_scope = graph_call_scope{*invoke_.owned_call_options,
                                        invoke_.bound_call_scope.prefix()};
  if (invoke_.bound_call_scope.trace().has_value()) {
    rebound_scope = rebound_scope.with_trace(*invoke_.bound_call_scope.trace());
  }
  invoke_.bound_call_scope = std::move(rebound_scope);
}

inline auto detail::invoke_runtime::start_graph_run(run_state &&state)
    -> graph_sender {
  if (state.init_error_.has_value()) {
    return state.immediate_failure(*state.init_error_);
  }
  if (state.owner_->options().mode == graph_runtime_mode::dag) {
    auto dag_state = detail::invoke_runtime::dag_run_state{std::move(state)};
    dag_state.initialize_dag_entry();
    if (dag_state.init_error_.has_value()) {
      return dag_state.immediate_failure(*dag_state.init_error_);
    }
    return graph_sender{
        detail::invoke_runtime::dag_run_sender{std::move(dag_state)}};
  }
  auto pregel_state =
      detail::invoke_runtime::pregel_run_state{std::move(state)};
  pregel_state.initialize_pregel_entry();
  if (pregel_state.init_error_.has_value()) {
    return pregel_state.immediate_failure(*pregel_state.init_error_);
  }
  return graph_sender{
      detail::invoke_runtime::pregel_run_sender{std::move(pregel_state)}};
}

} // namespace wh::compose
