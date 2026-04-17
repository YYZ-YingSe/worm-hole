// Defines graph-specific error detail helpers with phase/path diagnostics.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "wh/compose/node.hpp"
#include "wh/compose/types.hpp"
#include "wh/core/error.hpp"

namespace wh::compose {

/// Runtime phase where one compose error was produced.
enum class compose_error_phase : std::uint8_t {
  /// Graph compile-time validation and index build.
  compile = 0U,
  /// Graph runtime scheduling/readiness resolution.
  schedule,
  /// Graph runtime node execute/evaluate path.
  execute,
  /// Checkpoint capture/load/restore path.
  checkpoint,
  /// Resume/replay path.
  resume,
  /// Graph diff/restore validation path.
  validate,
};

/// Structured detail emitted when runtime exceeds configured step budget.
struct graph_step_limit_error_detail {
  /// Step index that exceeded budget.
  std::size_t step{0U};
  /// Effective step budget used for this run.
  std::size_t budget{0U};
  /// Node key being evaluated when overflow happened.
  std::string node{};
  /// Completed node set captured at overflow point.
  std::vector<std::string> completed_node_keys{};
};

/// Structured detail emitted when one node execution exceeds timeout budget.
struct graph_node_timeout_error_detail {
  /// Node key that timed out.
  std::string node{};
  /// Attempt index (0-based) where timeout was observed.
  std::size_t attempt{0U};
  /// Effective timeout budget.
  std::chrono::milliseconds timeout{0};
  /// Measured elapsed execution time.
  std::chrono::milliseconds elapsed{0};
};

/// Structured node-execution error preserving NodePath and raw error.
struct graph_node_run_error_detail {
  /// Runtime node path captured at failure point.
  node_path path{};
  /// Stable node key.
  std::string node{};
  /// Wrapped node-run error code.
  wh::core::error_code code{wh::core::errc::ok};
  /// Raw node error code returned by underlying node implementation.
  wh::core::error_code raw_error{wh::core::errc::ok};
  /// Human-readable detail message.
  std::string message{};
};

/// Structured graph-runtime error preserving failing phase and optional path.
struct graph_run_error_detail {
  /// Runtime phase where error was produced.
  compose_error_phase phase{compose_error_phase::execute};
  /// Optional runtime node path if error can be attributed to one node.
  std::optional<node_path> path{};
  /// Optional stable node key.
  std::string node{};
  /// Wrapped graph-run error code.
  wh::core::error_code code{wh::core::errc::ok};
  /// Optional raw lower-level error code.
  std::optional<wh::core::error_code> raw_error{};
  /// Human-readable detail message.
  std::string message{};
};

/// Structured stream-read error used by stream-reader aggregation wrappers.
struct graph_new_stream_read_error_detail {
  /// Runtime node path captured when stream read failed.
  node_path path{};
  /// Stable node key.
  std::string node{};
  /// Wrapped stream-read error code.
  wh::core::error_code code{wh::core::errc::ok};
  /// Raw stream error code.
  wh::core::error_code raw_error{wh::core::errc::ok};
  /// Human-readable detail message.
  std::string message{};
};

/// Structured compose error payload used by introspection/diagnostics.
struct compose_error {
  /// Structured error code.
  wh::core::error_code code{wh::core::errc::ok};
  /// Runtime phase where this error originated.
  compose_error_phase phase{compose_error_phase::execute};
  /// Stable node key/path if available.
  std::string node{};
  /// Human-readable detail message.
  std::string message{};
  /// Optional lower-level root cause.
  std::optional<wh::core::error_code> root_cause{};
};

/// Converts step-limit detail to one compose error record.
[[nodiscard]] inline auto
to_compose_error(const graph_step_limit_error_detail &detail) -> compose_error {
  compose_error error{};
  error.code = wh::core::errc::timeout;
  error.phase = compose_error_phase::schedule;
  error.node = detail.node;
  error.message = "step-limit-exceeded:step=" + std::to_string(detail.step) +
                  ";budget=" + std::to_string(detail.budget);
  return error;
}

/// Converts node-timeout detail to one compose error record.
[[nodiscard]] inline auto
to_compose_error(const graph_node_timeout_error_detail &detail)
    -> compose_error {
  compose_error error{};
  error.code = wh::core::errc::timeout;
  error.phase = compose_error_phase::execute;
  error.node = detail.node;
  error.message = "node-timeout:attempt=" + std::to_string(detail.attempt) +
                  ";timeout_ms=" + std::to_string(detail.timeout.count()) +
                  ";elapsed_ms=" + std::to_string(detail.elapsed.count());
  return error;
}

/// Converts one compose error to graph diagnostic record.
[[nodiscard]] inline auto to_graph_diagnostic(const compose_error &error)
    -> graph_diagnostic {
  std::string phase{};
  switch (error.phase) {
  case compose_error_phase::compile:
    phase = "compile";
    break;
  case compose_error_phase::schedule:
    phase = "schedule";
    break;
  case compose_error_phase::execute:
    phase = "execute";
    break;
  case compose_error_phase::checkpoint:
    phase = "checkpoint";
    break;
  case compose_error_phase::resume:
    phase = "resume";
    break;
  case compose_error_phase::validate:
    phase = "validate";
    break;
  }

  std::string message{};
  message.reserve(64U + error.node.size() + error.message.size());
  message += "phase=";
  message += phase;
  if (!error.node.empty()) {
    message += ";node=";
    message += error.node;
  }
  if (!error.message.empty()) {
    message += ";message=";
    message += error.message;
  }
  if (error.root_cause.has_value()) {
    message += ";root_cause=";
    message += std::to_string(static_cast<int>(error.root_cause->value()));
  }
  return graph_diagnostic{error.code, std::move(message)};
}

} // namespace wh::compose
