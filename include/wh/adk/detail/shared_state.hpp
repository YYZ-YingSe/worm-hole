// Defines helpers for storing one graph-wide runtime state object in the
// parent process-state instead of transient node-local slots.
#pragma once

#include <utility>

#include "wh/compose/runtime/state.hpp"
#include "wh/core/result.hpp"

namespace wh::adk::detail {

[[nodiscard]] inline auto
shared_process_state(wh::compose::graph_process_state &process_state) noexcept
    -> wh::compose::graph_process_state & {
  return process_state.workflow_scope_root();
}

template <typename state_t>
[[nodiscard]] inline auto shared_state_ref(wh::compose::graph_process_state &process_state)
    -> wh::core::result<std::reference_wrapper<state_t>> {
  return process_state.template workflow_state_ref<state_t>();
}

template <typename state_t, typename... args_t>
inline auto emplace_shared_state(wh::compose::graph_process_state &process_state, args_t &&...args)
    -> wh::core::result<std::reference_wrapper<state_t>> {
  return process_state.template emplace_workflow_state<state_t>(std::forward<args_t>(args)...);
}

} // namespace wh::adk::detail
