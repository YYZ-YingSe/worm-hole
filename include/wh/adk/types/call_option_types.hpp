// Defines ADK call-option support types shared by authoring, flow mapping,
// and runtime-scope materialization.
#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "wh/core/any.hpp"
#include "wh/core/type_traits.hpp"

namespace wh::adk {

/// Transparent hash alias used by ADK option dictionaries.
using option_string_hash = wh::core::transparent_string_hash;

/// Transparent equality alias used by ADK option dictionaries.
using option_string_equal = wh::core::transparent_string_equal;

/// Type-erased option bag shared by ADK and Flow surfaces.
using option_bag = std::unordered_map<std::string, wh::core::any,
                                      option_string_hash, option_string_equal>;

/// Named option bag used for one agent/tool targeted scope.
struct named_option_bag {
  /// Stable target name for this scoped bag.
  std::string name{};
  /// Options visible only to `name`.
  option_bag values{};
};

/// Transfer-history trim knobs applied at agent/tool bridge boundaries.
struct transfer_trim_options {
  /// When set, transfer assistant messages are dropped on the targeted path.
  std::optional<bool> trim_assistant_transfer_message{};
  /// When set, transfer tool-call/request pairs are dropped on the targeted path.
  std::optional<bool> trim_tool_transfer_pair{};
};

/// Resolved transfer trim knobs after layer overlay.
struct resolved_transfer_trim_options {
  /// True means assistant transfer messages must be trimmed.
  bool trim_assistant_transfer_message{false};
  /// True means transfer tool-call/request pairs must be trimmed.
  bool trim_tool_transfer_pair{false};
};

/// Agent/flow level budget and circuit-breaker knobs.
struct call_budget_options {
  /// Optional maximum concurrent fanout/tool work for one call tree.
  std::optional<std::size_t> max_concurrency{};
  /// Optional maximum iteration/step budget for one authored loop.
  std::optional<std::size_t> max_iterations{};
  /// Optional token budget used by governance/reduction layers.
  std::optional<std::size_t> token_budget{};
  /// Optional breaker threshold for repeated failures.
  std::optional<std::size_t> circuit_breaker_threshold{};
  /// Optional timeout budget applied at the call surface.
  std::optional<std::chrono::milliseconds> timeout{};
  /// Optional fail-fast flag for subtree short-circuit.
  std::optional<bool> fail_fast{};
};

} // namespace wh::adk
