#pragma once

#include <algorithm>
#include <iterator>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include "wh/core/resume_state.hpp"

namespace wh::adk {

using interrupt_info = wh::core::interrupt_context;
using interrupt_signal = wh::core::interrupt_signal;
using resume_info = wh::core::resume_state;

[[nodiscard]] inline auto
from_interrupt_contexts(const std::span<const interrupt_info> contexts)
    -> std::vector<interrupt_signal> {
  std::vector<interrupt_signal> signals{};
  signals.reserve(contexts.size());
  const auto transformed = std::ranges::transform(
      contexts, std::back_inserter(signals),
      [](const interrupt_info &context) {
        return wh::core::to_interrupt_signal(context);
      });
  static_cast<void>(transformed);
  return signals;
}

[[nodiscard]] inline auto
from_interrupt_contexts(std::vector<interrupt_info> &&contexts)
    -> std::vector<interrupt_signal> {
  std::vector<interrupt_signal> signals{};
  signals.reserve(contexts.size());
  for (auto &context : contexts) {
    signals.push_back(wh::core::to_interrupt_signal(std::move(context)));
  }
  return signals;
}

[[nodiscard]] inline auto
to_interrupt_contexts(const std::span<const interrupt_signal> signals)
    -> std::vector<interrupt_info> {
  std::vector<interrupt_info> contexts{};
  contexts.reserve(signals.size());
  const auto transformed = std::ranges::transform(
      signals, std::back_inserter(contexts),
      [](const interrupt_signal &signal) {
        return wh::core::to_interrupt_context(signal);
      });
  static_cast<void>(transformed);
  return contexts;
}

[[nodiscard]] inline auto
to_interrupt_contexts(std::vector<interrupt_signal> &&signals)
    -> std::vector<interrupt_info> {
  std::vector<interrupt_info> contexts{};
  contexts.reserve(signals.size());
  for (auto &signal : signals) {
    contexts.push_back(wh::core::to_interrupt_context(std::move(signal)));
  }
  return contexts;
}

} // namespace wh::adk
