// Defines scheduling/runtime scope helpers extracted from graph execution core.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "wh/compose/graph/call_options.hpp"
#include "wh/compose/graph/compile_options.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"

namespace wh::compose::detail::schedule_runtime {

inline constexpr std::string_view graph_cache_scope_session_key =
    "compose.graph.cache.key";
inline constexpr std::string_view graph_pregel_max_steps_session_key =
    "compose.pregel.max_steps";

[[nodiscard]] inline auto resolve_cache_scope(
    const wh::core::run_context &context, const graph_call_options &call_options,
    const graph_compile_options &options) -> std::string {
  std::string_view runtime_scope{};
  if (call_options.cache_key.has_value()) {
    runtime_scope = *call_options.cache_key;
  } else {
    auto scope_ref = wh::core::session_value_ref<std::string>(
        context, graph_cache_scope_session_key);
    if (!scope_ref.has_error()) {
      runtime_scope = scope_ref.value().get();
    }
  }
  if (runtime_scope.empty()) {
    runtime_scope = options.name;
  }
  return std::string{runtime_scope};
}

[[nodiscard]] inline auto resolve_step_budget(
    const wh::core::run_context &context, const graph_call_options &call_options,
    const graph_compile_options &options) -> wh::core::result<std::size_t> {
  if (options.mode != graph_runtime_mode::pregel) {
    if (call_options.pregel_max_steps.has_value()) {
      return wh::core::result<std::size_t>::failure(
          wh::core::errc::contract_violation);
    }
    return options.max_steps;
  }

  if (call_options.pregel_max_steps.has_value()) {
    if (*call_options.pregel_max_steps == 0U) {
      return wh::core::result<std::size_t>::failure(
          wh::core::errc::invalid_argument);
    }
    return *call_options.pregel_max_steps;
  }

  auto override_budget = wh::core::session_value_ref<std::size_t>(
      context, graph_pregel_max_steps_session_key);
  if (override_budget.has_value() && override_budget.value().get() > 0U) {
    return override_budget.value().get();
  }
  if (options.max_steps == 0U) {
    return wh::core::result<std::size_t>::failure(wh::core::errc::invalid_argument);
  }
  return options.max_steps;
}

} // namespace wh::compose::detail::schedule_runtime
