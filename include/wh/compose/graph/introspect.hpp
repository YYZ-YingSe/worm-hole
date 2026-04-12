// Defines compose runtime introspection events with path/phase/root-cause
// fields.
#pragma once

#include <string>

#include "wh/compose/graph/error.hpp"
#include "wh/compose/node.hpp"
#include "wh/compose/types.hpp"

namespace wh::compose {

/// Introspection event emitted by compose compile/run/checkpoint flows.
struct graph_introspect_event {
  /// Target node path captured for this event.
  node_path path{};
  /// Runtime phase where this event was emitted.
  compose_error_phase phase{compose_error_phase::execute};
  /// Structured status code.
  wh::core::error_code code{wh::core::errc::ok};
  /// Root-cause summary text (if available).
  std::string root_cause{};
  /// Human-readable event message.
  std::string message{};
};

/// Converts one introspection event to graph diagnostic record.
[[nodiscard]] inline auto
to_graph_diagnostic(const graph_introspect_event &event) -> graph_diagnostic {
  compose_error error{};
  error.code = event.code;
  error.phase = event.phase;
  error.node = event.path.to_string();
  error.message = event.message;
  if (!event.root_cause.empty()) {
    error.message += ";root_cause=" + event.root_cause;
  }
  return to_graph_diagnostic(error);
}

} // namespace wh::compose
