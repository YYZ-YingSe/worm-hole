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
    -> wh::core::result<wh::core::interrupt_signal> {
  auto owned_interrupt = wh::core::into_owned(interrupt);
  if (owned_interrupt.has_error()) {
    return wh::core::result<wh::core::interrupt_signal>::failure(
        owned_interrupt.error());
  }
  auto materialized = std::move(owned_interrupt).value();
  return wh::core::interrupt_signal{std::move(materialized.interrupt_id),
                                    std::move(materialized.location),
                                    std::move(materialized.payload),
                                    wh::core::any{}, false};
}

/// Converts movable tool interrupt to core interrupt signal.
[[nodiscard]] inline auto to_interrupt_signal(tool_interrupt &&interrupt)
    -> wh::core::interrupt_signal {
  return wh::core::interrupt_signal{std::move(interrupt.interrupt_id),
                                    std::move(interrupt.location),
                                    std::move(interrupt.payload),
                                    wh::core::any{}, false};
}

/// Converts core interrupt signal back to tool interrupt.
[[nodiscard]] inline auto
from_interrupt_signal(const wh::core::interrupt_signal &signal)
    -> wh::core::result<tool_interrupt> {
  auto payload = wh::core::into_owned(signal.state);
  if (payload.has_error()) {
    return wh::core::result<tool_interrupt>::failure(payload.error());
  }
  return tool_interrupt{signal.interrupt_id, signal.location,
                        std::move(payload).value()};
}

/// Converts movable core interrupt signal back to tool interrupt.
[[nodiscard]] inline auto from_interrupt_signal(wh::core::interrupt_signal &&signal)
    -> tool_interrupt {
  return tool_interrupt{std::move(signal.interrupt_id), std::move(signal.location),
                        std::move(signal.state)};
}

/// Checks whether interrupt location matches current resume target.
[[nodiscard]] inline auto is_resume_target(const tool_interrupt &interrupt,
                                           const wh::core::resume_state &state,
                                           const bool exact = false) noexcept
    -> bool {
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
[[nodiscard]] inline auto
infer_root_cause(const std::span<const wh::core::error_code> causes)
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
[[nodiscard]] inline auto aggregate_interrupts(
    const std::span<const tool_interrupt> interrupts,
    const std::optional<wh::core::error_code> &root_cause = std::nullopt)
    -> wh::core::result<aggregated_interrupts> {
  if (interrupts.empty()) {
    return wh::core::result<aggregated_interrupts>::failure(
        wh::core::errc::not_found);
  }

  aggregated_interrupts aggregated{};
  aggregated.interrupts.reserve(interrupts.size());
  for (const auto &interrupt : interrupts) {
    auto owned_interrupt = wh::core::into_owned(interrupt);
    if (owned_interrupt.has_error()) {
      return wh::core::result<aggregated_interrupts>::failure(
          owned_interrupt.error());
    }
    aggregated.interrupts.push_back(std::move(owned_interrupt).value());
  }
  aggregated.root_cause = root_cause;
  return aggregated;
}

} // namespace wh::tool

namespace wh::core {

template <> struct any_owned_traits<wh::tool::aggregated_interrupts> {
  [[nodiscard]] static auto into_owned(const wh::tool::aggregated_interrupts &value)
      -> wh::core::result<wh::tool::aggregated_interrupts> {
    wh::tool::aggregated_interrupts owned{};
    owned.interrupts.reserve(value.interrupts.size());
    for (const auto &interrupt : value.interrupts) {
      auto owned_interrupt = wh::core::into_owned(interrupt);
      if (owned_interrupt.has_error()) {
        return wh::core::result<wh::tool::aggregated_interrupts>::failure(
            owned_interrupt.error());
      }
      owned.interrupts.push_back(std::move(owned_interrupt).value());
    }
    owned.root_cause = value.root_cause;
    return owned;
  }

  [[nodiscard]] static auto into_owned(wh::tool::aggregated_interrupts &&value)
      -> wh::core::result<wh::tool::aggregated_interrupts> {
    wh::tool::aggregated_interrupts owned{};
    owned.interrupts.reserve(value.interrupts.size());
    for (auto &interrupt : value.interrupts) {
      auto owned_interrupt = wh::core::into_owned(std::move(interrupt));
      if (owned_interrupt.has_error()) {
        return wh::core::result<wh::tool::aggregated_interrupts>::failure(
            owned_interrupt.error());
      }
      owned.interrupts.push_back(std::move(owned_interrupt).value());
    }
    owned.root_cause = value.root_cause;
    return owned;
  }
};

template <> struct any_owned_traits<wh::tool::tool_interrupt> {
  [[nodiscard]] static auto into_owned(const wh::tool::tool_interrupt &value)
      -> wh::core::result<wh::tool::tool_interrupt> {
    auto payload = wh::core::into_owned(value.payload);
    if (payload.has_error()) {
      return wh::core::result<wh::tool::tool_interrupt>::failure(payload.error());
    }
    return wh::tool::tool_interrupt{
        .interrupt_id = value.interrupt_id,
        .location = value.location,
        .payload = std::move(payload).value(),
    };
  }

  [[nodiscard]] static auto into_owned(wh::tool::tool_interrupt &&value)
      -> wh::core::result<wh::tool::tool_interrupt> {
    auto payload = wh::core::into_owned(std::move(value.payload));
    if (payload.has_error()) {
      return wh::core::result<wh::tool::tool_interrupt>::failure(payload.error());
    }
    return wh::tool::tool_interrupt{
        .interrupt_id = std::move(value.interrupt_id),
        .location = std::move(value.location),
        .payload = std::move(payload).value(),
    };
  }
};

} // namespace wh::core
