#pragma once

#include "wh/compose/graph/detail/run_sender.hpp"
#include "wh/compose/graph/detail/runtime/dag_runtime.hpp"
#include "wh/compose/graph/detail/runtime/pregel_runtime.hpp"
#include "wh/compose/graph/graph.hpp"
#include "wh/compose/node/detail/runtime_access.hpp"

namespace wh::compose {

inline detail::invoke_runtime::invoke_session::invoke_session(
    const graph *owner, graph_value &&input, wh::core::run_context &context,
    graph_call_options &&call_options, wh::core::detail::any_resume_scheduler_t control_scheduler,
    wh::core::detail::any_resume_scheduler_t work_scheduler, node_path path_prefix,
    graph_process_state *parent_process_state,
    detail::runtime_state::invoke_outputs *nested_outputs, const graph_runtime_services *services,
    graph_invoke_controls controls, detail::runtime_state::invoke_outputs *published_outputs,
    const invoke_session *parent_state)
    : owner_(owner), context_(context) {
  invoke_.controls = std::move(controls);
  invoke_.services = services;
  invoke_.parent_state = parent_state;
  invoke_.path_prefix = std::move(path_prefix);
  invoke_.parent_process_state = parent_process_state;
  invoke_.nested_outputs = nested_outputs;
  invoke_.published_outputs = published_outputs;
  invoke_.control_scheduler.emplace(std::move(control_scheduler));
  invoke_.work_scheduler.emplace(std::move(work_scheduler));
  invoke_.owned_call_options = std::make_unique<graph_call_options>(std::move(call_options));
  initialize(std::move(input), graph_call_scope{*invoke_.owned_call_options});
}

inline detail::invoke_runtime::invoke_session::invoke_session(
    const graph *owner, graph_value &&input, wh::core::run_context &context,
    graph_call_scope call_scope, wh::core::detail::any_resume_scheduler_t control_scheduler,
    wh::core::detail::any_resume_scheduler_t work_scheduler, node_path path_prefix,
    graph_process_state *parent_process_state,
    detail::runtime_state::invoke_outputs *nested_outputs, const graph_runtime_services *services,
    graph_invoke_controls controls, detail::runtime_state::invoke_outputs *published_outputs,
    const invoke_session *parent_state)
    : owner_(owner), context_(context) {
  invoke_.controls = std::move(controls);
  invoke_.services = services;
  invoke_.parent_state = parent_state;
  invoke_.path_prefix = std::move(path_prefix);
  invoke_.parent_process_state = parent_process_state;
  invoke_.nested_outputs = nested_outputs;
  invoke_.published_outputs = published_outputs;
  invoke_.control_scheduler.emplace(std::move(control_scheduler));
  invoke_.work_scheduler.emplace(std::move(work_scheduler));
  initialize(std::move(input), std::move(call_scope));
}

inline auto detail::invoke_runtime::invoke_session::rebind_moved_runtime_storage() noexcept
    -> void {
  if (invoke_.parent_state == nullptr) {
    invoke_.forwarded_checkpoints = std::addressof(invoke_.owned_forwarded_checkpoints);
  }
  if (invoke_.owned_call_options == nullptr) {
    return;
  }
  auto rebound_scope =
      graph_call_scope{*invoke_.owned_call_options, invoke_.bound_call_scope.prefix()};
  if (invoke_.bound_call_scope.trace().has_value()) {
    rebound_scope = rebound_scope.with_trace(*invoke_.bound_call_scope.trace());
  }
  invoke_.bound_call_scope = std::move(rebound_scope);
}

template <typename sender_t, typename runtime_t>
inline auto detail::invoke_runtime::start_initialized_runtime(runtime_t runtime) -> graph_sender {
  auto &session = runtime.session();
  if (session.init_error_.has_value()) {
    runtime.try_persist_checkpoint();
    return session.immediate_failure(*session.init_error_);
  }
  runtime.initialize_entry();
  if (session.init_error_.has_value()) {
    runtime.try_persist_checkpoint();
    return session.immediate_failure(*session.init_error_);
  }
  return graph_sender{sender_t{std::move(runtime)}};
}

inline auto detail::invoke_runtime::start_dag_run(invoke_session &&session) -> graph_sender {
  return start_initialized_runtime<dag_run_sender>(
      detail::invoke_runtime::dag_runtime{std::move(session)});
}

inline auto detail::invoke_runtime::start_pregel_run(invoke_session &&session) -> graph_sender {
  return start_initialized_runtime<pregel_run_sender>(
      detail::invoke_runtime::pregel_runtime{std::move(session)});
}

inline auto detail::start_session(const graph &graph,
                                  detail::invoke_runtime::invoke_session session) -> graph_sender {
  return graph.options().mode == graph_runtime_mode::dag
             ? detail::invoke_runtime::start_dag_run(std::move(session))
             : detail::invoke_runtime::start_pregel_run(std::move(session));
}

template <typename scope_t>
  requires std::same_as<std::remove_cvref_t<scope_t>, graph_call_options> ||
           std::same_as<std::remove_cvref_t<scope_t>, graph_call_scope>
inline auto detail::start_session(const graph &graph, wh::core::run_context &context,
                                  graph_value &&input, scope_t &&call_scope,
                                  const wh::core::detail::any_resume_scheduler_t &control_scheduler,
                                  const wh::core::detail::any_resume_scheduler_t &work_scheduler,
                                  node_path path_prefix, graph_process_state *parent_process_state,
                                  detail::runtime_state::invoke_outputs *nested_outputs,
                                  const graph_runtime_services *services,
                                  graph_invoke_controls controls,
                                  detail::runtime_state::invoke_outputs *published_outputs,
                                  const detail::invoke_runtime::invoke_session *parent_state)
    -> graph_sender {
  return start_session(graph, detail::invoke_runtime::invoke_session{
                                  std::addressof(graph), std::move(input), context,
                                  std::forward<scope_t>(call_scope),
                                  wh::core::detail::any_resume_scheduler_t{control_scheduler},
                                  wh::core::detail::any_resume_scheduler_t{work_scheduler},
                                  std::move(path_prefix), parent_process_state, nested_outputs,
                                  services, std::move(controls), published_outputs, parent_state});
}

inline auto detail::start_request(const graph &graph, wh::core::run_context &context,
                                  graph_invoke_request request,
                                  const wh::core::detail::any_resume_scheduler_t &control_scheduler,
                                  const wh::core::detail::any_resume_scheduler_t &work_scheduler,
                                  detail::runtime_state::invoke_outputs *published_outputs)
    -> graph_sender {
  auto controls = std::move(request.controls);
  auto call_options = std::move(controls.call);
  const bool has_restore_source =
      controls.checkpoint.load.has_value() || !controls.checkpoint.forwarded_once.empty();
  auto input = detail::normalize_graph_input(graph.boundary().input, has_restore_source,
                                             std::move(request.input));
  if (input.has_error()) {
    return detail::failure_graph_sender(input.error());
  }
  return start_session(graph, context, std::move(input).value(), std::move(call_options),
                       control_scheduler, work_scheduler, {}, nullptr, nullptr, request.services,
                       std::move(controls), published_outputs, nullptr);
}

inline auto
detail::start_bound_graph(const graph &graph, wh::core::run_context &context, graph_value &input,
                          const graph_call_options *call_options, const node_path *path_prefix,
                          graph_process_state *parent_process_state,
                          detail::runtime_state::invoke_outputs *nested_outputs,
                          const wh::core::detail::any_resume_scheduler_t &control_scheduler,
                          const wh::core::detail::any_resume_scheduler_t &work_scheduler,
                          const detail::invoke_runtime::invoke_session *parent_state,
                          const graph_runtime_services *services, graph_invoke_controls controls)
    -> graph_sender {
  auto bound_call_options =
      call_options != nullptr ? graph_call_options{*call_options} : graph_call_options{};
  auto bound_path_prefix = path_prefix != nullptr ? node_path{*path_prefix} : node_path{};
  return start_session(graph, context, std::move(input), std::move(bound_call_options),
                       control_scheduler, work_scheduler, std::move(bound_path_prefix),
                       parent_process_state, nested_outputs, services, std::move(controls), nullptr,
                       parent_state);
}

inline auto
detail::start_scoped_graph(const graph &graph, wh::core::run_context &context, graph_value &input,
                           const graph_call_scope *call_scope, const node_path *path_prefix,
                           graph_process_state *parent_process_state,
                           detail::runtime_state::invoke_outputs *nested_outputs,
                           const wh::core::detail::any_resume_scheduler_t &control_scheduler,
                           const wh::core::detail::any_resume_scheduler_t &work_scheduler,
                           const detail::invoke_runtime::invoke_session *parent_state,
                           const graph_runtime_services *services, graph_invoke_controls controls)
    -> graph_sender {
  auto bound_call_scope = call_scope != nullptr ? *call_scope : graph_call_scope{};
  auto bound_path_prefix = path_prefix != nullptr ? node_path{*path_prefix} : node_path{};
  return start_session(graph, context, std::move(input), std::move(bound_call_scope),
                       control_scheduler, work_scheduler, std::move(bound_path_prefix),
                       parent_process_state, nested_outputs, services, std::move(controls), nullptr,
                       parent_state);
}

inline auto detail::start_nested_graph(const graph &graph, wh::core::run_context &context,
                                       graph_value &input, const node_runtime &runtime)
    -> graph_sender {
  return detail::node_runtime_access::nested_entry(runtime)(
      graph, context, input, runtime.call_options(), runtime.path(), runtime.process_state(),
      detail::node_runtime_access::invoke_outputs(runtime), runtime.trace());
}

} // namespace wh::compose
