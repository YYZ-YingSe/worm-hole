#pragma once

#include <concepts>
#include <stop_token>
#include <type_traits>
#include <utility>

#include "wh/async/completion_token_types.hpp"

namespace wh::core {

inline constexpr use_sender_t use_sender{};
inline constexpr use_awaitable_t use_awaitable{};

template <typename handler_t>
[[nodiscard]] auto
use_callback(handler_t &&handler,
             const std::stop_token stop_token = std::stop_token{})
    -> use_callback_t<std::decay_t<handler_t>> {
  return use_callback_t<std::decay_t<handler_t>>{
      std::forward<handler_t>(handler), stop_token};
}

template <typename token_t>
concept completion_token =
    std::same_as<std::remove_cvref_t<token_t>, use_sender_t> ||
    std::same_as<std::remove_cvref_t<token_t>, use_awaitable_t> ||
    callback_token<std::remove_cvref_t<token_t>>;

} // namespace wh::core
