// Defines graph compile-option contracts used by graph compile/snapshot flows.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "wh/compose/graph/compile_info.hpp"
#include "wh/compose/graph/policy.hpp"
#include "wh/compose/types.hpp"
#include "wh/core/function.hpp"
#include "wh/core/result.hpp"

namespace wh::compose {

/// Graph-level compile callback receiving immutable compile graph info snapshot.
using graph_compile_callback = wh::core::callback_function<
    wh::core::result<void>(const graph_compile_info &) const>;

/// Immutable compile options snapshot bound to one compiled graph definition.
struct graph_compile_options {
  /// Human-readable graph name for diagnostics and callback run-info.
  std::string name{"graph"};
  /// Compile-visible graph boundary contract.
  graph_boundary boundary{};
  /// Runtime mode that controls cycle and step-budget behavior.
  graph_runtime_mode mode{graph_runtime_mode::dag};
  /// Eager dispatch flag controlling immediate/deferred dependent scheduling.
  bool eager{true};
  /// Default max step budget used by basic invoke loop.
  std::size_t max_steps{1024U};
  /// Keeps compile-time cold structures for diagnostics after compile.
  bool retain_cold_data{true};
  /// Graph-level trigger policy.
  graph_trigger_mode trigger_mode{graph_trigger_mode::any_predecessor};
  /// Graph-level fan-in policy.
  graph_fan_in_policy fan_in_policy{graph_fan_in_policy::allow_partial};
  /// Graph-level default retry budget.
  std::size_t retry_budget{0U};
  /// Graph-level default node timeout budget (`nullopt` means no timeout).
  std::optional<std::chrono::milliseconds> node_timeout{};
  /// Graph-level global parallel activation gate (`>=1`).
  std::size_t max_parallel_nodes{1U};
  /// Graph-level default per-node parallel gate (`>=1`).
  std::size_t max_parallel_per_node{1U};
  /// Enables node-local state generation/handler capability for this graph.
  bool enable_local_state_generation{true};
  /// Optional compile callback invoked once compile snapshot is finalized.
  graph_compile_callback compile_callback{nullptr};
};

/// Serializes compile-options snapshot into one stable diagnostic string.
[[nodiscard]] inline auto
serialize_graph_compile_options(const graph_compile_options &options)
    -> std::string {
  std::string text{};
  text.reserve(192U + options.name.size());
  text += "name=";
  text += options.name;
  text += ";boundary=";
  text += std::string{to_string(options.boundary.input)};
  text += "->";
  text += std::string{to_string(options.boundary.output)};
  text += ";mode=";
  text += options.mode == graph_runtime_mode::pregel ? "pregel" : "dag";
  text += ";eager=";
  text += options.eager ? "true" : "false";
  text += ";max_steps=";
  text += std::to_string(options.max_steps);
  text += ";retain_cold_data=";
  text += options.retain_cold_data ? "true" : "false";
  text += ";trigger_mode=";
  text += options.trigger_mode == graph_trigger_mode::all_predecessors
              ? "all_predecessors"
              : "any_predecessor";
  text += ";fan_in_policy=";
  if (options.fan_in_policy == graph_fan_in_policy::require_all_sources) {
    text += "require_all_sources";
  } else if (options.fan_in_policy ==
             graph_fan_in_policy::require_all_sources_with_eof) {
    text += "require_all_sources_with_eof";
  } else {
    text += "allow_partial";
  }
  text += ";retry_budget=";
  text += std::to_string(options.retry_budget);
  text += ";node_timeout_ms=";
  if (options.node_timeout.has_value()) {
    text += std::to_string(options.node_timeout->count());
  } else {
    text += "none";
  }
  text += ";max_parallel_nodes=";
  text += std::to_string(options.max_parallel_nodes);
  text += ";max_parallel_per_node=";
  text += std::to_string(options.max_parallel_per_node);
  text += ";enable_local_state_generation=";
  text += options.enable_local_state_generation ? "true" : "false";
  text += ";compile_callback=";
  text += static_cast<bool>(options.compile_callback) ? "true" : "false";
  return text;
}

} // namespace wh::compose
