// Provides declarations and utilities for `wh/tool/callback_event.hpp`.
#pragma once

#include <string>

namespace wh::tool {

/// Structured callback payload for tool lifecycle events.
struct tool_callback_event {
  /// Tool name being invoked/streamed.
  std::string tool_name{};
  /// Raw JSON input payload passed to tool.
  std::string input_json{};
  /// Text output returned by tool (invoke path or summarized stream result).
  std::string output_text{};
  /// Structured context for validation/timeout/cancellation failures.
  std::string error_context{};
  /// True when terminal error maps to cancellation/interrupt semantics.
  bool interrupted{false};
};

} // namespace wh::tool
