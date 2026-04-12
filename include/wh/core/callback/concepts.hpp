// Defines callback-related concepts used to validate lifecycle callback
// signatures across component option and callback APIs.
#pragma once

#include <concepts>
#include <utility>

#include "wh/core/callback/types.hpp"

namespace wh::core {

/// Predicate concept for stage-level callback timing filters.
template <typename timing_checker_t>
concept TimingChecker =
    requires(const timing_checker_t checker, const callback_stage stage) {
      { checker(stage) } -> std::convertible_to<bool>;
    };

/// Callback that consumes `(stage, event_view)`.
template <typename callback_t>
concept StageViewCallbackLike =
    requires(callback_t callback, const callback_stage stage,
             const callback_event_view event, const callback_run_info &info) {
      { callback(stage, event, info) } -> std::same_as<void>;
    };

/// Callback that consumes `(stage, owning_event_payload)`.
template <typename callback_t>
concept StagePayloadCallbackLike =
    requires(callback_t callback, const callback_stage stage,
             callback_event_payload payload, const callback_run_info &info) {
      { callback(stage, std::move(payload), info) } -> std::same_as<void>;
    };

/// Configuration object expected by callback registration.
template <typename config_t>
concept CallbackConfigLike =
    requires(const config_t config, const callback_stage stage) {
      { config.name } -> std::convertible_to<std::string>;
      { config.timing_checker(stage) } -> std::convertible_to<bool>;
    };

} // namespace wh::core
