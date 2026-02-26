#pragma once

#include <concepts>

#include "wh/core/types/callback_types.hpp"

namespace wh::core {

template <typename timing_checker_t>
concept TimingChecker =
    requires(const timing_checker_t checker, const callback_stage stage) {
      { checker(stage) } -> std::convertible_to<bool>;
    };

template <typename handler_t>
concept CallbackHandlerLike =
    requires(handler_t handler, const callback_stage stage,
             const callback_event_view event) {
      { handler(stage, event) } -> std::same_as<void>;
    };

template <typename config_t>
concept CallbackHandlerConfigLike =
    requires(const config_t config, const callback_stage stage) {
      { config.name } -> std::convertible_to<std::string>;
      { config.timing_checker(stage) } -> std::convertible_to<bool>;
    };

} // namespace wh::core
