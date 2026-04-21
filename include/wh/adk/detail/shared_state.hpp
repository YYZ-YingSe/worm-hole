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
  if (process_state.parent() != nullptr) {
    return *process_state.parent();
  }
  return process_state;
}

template <typename state_t>
[[nodiscard]] inline auto shared_state_ref(wh::compose::graph_process_state &process_state)
    -> wh::core::result<std::reference_wrapper<state_t>> {
  return shared_process_state(process_state).template get<state_t>();
}

template <typename state_t, typename... args_t>
inline auto emplace_shared_state(wh::compose::graph_process_state &process_state, args_t &&...args)
    -> wh::core::result<std::reference_wrapper<state_t>> {
  return shared_process_state(process_state)
      .template emplace<state_t>(std::forward<args_t>(args)...);
}

} // namespace wh::adk::detail
