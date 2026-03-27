// Provides declarations and utilities for `wh/tool/interrupt.hpp`.
#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "wh/core/any.hpp"
#include "wh/core/resume_state.hpp"

namespace wh::tool {

/// Tool-layer interrupt payload.
struct tool_interrupt {
  /// Stable interrupt identifier.
  std::string interrupt_id{};
  /// Interrupt location path in resume state graph.
  wh::core::address location{};
  /// Tool-specific payload captured at interrupt point.
  wh::core::any payload{};
};

/// Aggregated interrupt group with optional root-cause error.
struct aggregated_interrupts {
  /// Interrupt entries included in this aggregate.
  std::vector<tool_interrupt> interrupts{};
  /// First non-success error chosen as aggregate root cause.
  std::optional<wh::core::error_code> root_cause{};
};

/// Converts tool interrupt to core interrupt signal.
[[nodiscard]] inline auto to_interrupt_signal(const tool_interrupt &interrupt)
    -> wh::core::interrupt_signal {
  return wh::core::interrupt_signal{interrupt.interrupt_id, interrupt.location,
                                    interrupt.payload, wh::core::any{}, false};
}

/// Converts core interrupt signal back to tool interrupt.
[[nodiscard]] inline auto from_interrupt_signal(
    const wh::core::interrupt_signal &signal) -> tool_interrupt {
  return tool_interrupt{signal.interrupt_id, signal.location, signal.state};
}

/// Checks whether interrupt location matches current resume target.
[[nodiscard]] inline auto
is_resume_target(const tool_interrupt &interrupt,
                 const wh::core::resume_state &state,
                 const bool exact = false) noexcept -> bool {
  if (exact) {
    return state.is_exact_resume_target(interrupt.location);
  }
  return state.is_resume_target(interrupt.location);
}

/// Inserts interrupt payload into resume state.
[[nodiscard]] inline auto inject_resume_data(const tool_interrupt &interrupt,
                                             wh::core::resume_state &state)
    -> wh::core::result<void> {
  return state.upsert(interrupt.interrupt_id, interrupt.location,
                      interrupt.payload);
}

/// Selects first failing cause as root cause.
[[nodiscard]] inline auto infer_root_cause(
    const std::span<const wh::core::error_code> causes)
    -> std::optional<wh::core::error_code> {
  if (causes.empty()) {
    return std::nullopt;
  }
  const auto first_non_ok = std::ranges::find_if(
      causes, [](const wh::core::error_code cause) { return cause.failed(); });
  if (first_non_ok == causes.end()) {
    return std::nullopt;
  }
  return *first_non_ok;
}

/// Packs interrupts plus optional root-cause into aggregate payload.
[[nodiscard]] inline auto
aggregate_interrupts(const std::span<const tool_interrupt> interrupts,
                     const std::optional<wh::core::error_code> &root_cause =
                         std::nullopt)
    -> wh::core::result<aggregated_interrupts> {
  if (interrupts.empty()) {
    return wh::core::result<aggregated_interrupts>::failure(
        wh::core::errc::not_found);
  }

  aggregated_interrupts aggregated{};
  aggregated.interrupts.reserve(interrupts.size());
  for (const auto &interrupt : interrupts) {
    aggregated.interrupts.push_back(interrupt);
  }
  aggregated.root_cause = root_cause;
  return aggregated;
}

} // namespace wh::tool
