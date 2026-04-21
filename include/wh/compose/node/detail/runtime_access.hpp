// Defines internal access helpers that bind graph runtime state onto the
// public node-runtime view.
#pragma once

#include "wh/compose/node/execution.hpp"

namespace wh::compose::detail {

/// Internal-only binder for `node_runtime` storage.
struct node_runtime_access {
  static auto reset(node_runtime &runtime, const std::size_t parallel_gate = 0U) noexcept -> void {
    runtime.parallel_gate_ = parallel_gate;
    runtime.call_options_ = nullptr;
    runtime.path_ = nullptr;
    runtime.control_scheduler_ = nullptr;
    runtime.work_scheduler_ = nullptr;
    runtime.process_state_ = nullptr;
    runtime.observation_ = nullptr;
    runtime.trace_ = nullptr;
    runtime.invoke_outputs_ = nullptr;
    runtime.nested_entry_ = nested_graph_entry{};
  }

  static auto bind_scope(node_runtime &runtime, const graph_call_scope *call_options,
                         const node_path *path) noexcept -> void {
    runtime.call_options_ = call_options;
    runtime.path_ = path;
  }

  static auto bind_runtime(node_runtime &runtime,
                           const wh::core::detail::any_resume_scheduler_t *control_scheduler,
                           const wh::core::detail::any_resume_scheduler_t *work_scheduler,
                           graph_process_state *process_state,
                           const graph_resolved_node_observation *observation,
                           const graph_node_trace *trace) noexcept -> void {
    runtime.control_scheduler_ = control_scheduler;
    runtime.work_scheduler_ = work_scheduler;
    runtime.process_state_ = process_state;
    runtime.observation_ = observation;
    runtime.trace_ = trace;
  }

  static auto bind_internal(node_runtime &runtime,
                            detail::runtime_state::invoke_outputs *invoke_outputs,
                            nested_graph_entry nested_entry) noexcept -> void {
    runtime.invoke_outputs_ = invoke_outputs;
    runtime.nested_entry_ = std::move(nested_entry);
  }

  [[nodiscard]] static auto invoke_outputs(const node_runtime &runtime) noexcept
      -> detail::runtime_state::invoke_outputs * {
    return runtime.invoke_outputs_;
  }

  [[nodiscard]] static auto nested_entry(const node_runtime &runtime) noexcept
      -> const nested_graph_entry & {
    return runtime.nested_entry_;
  }
};

} // namespace wh::compose::detail
