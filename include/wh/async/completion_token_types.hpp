#pragma once

#include <stop_token>
#include <type_traits>

namespace wh::core {

struct use_sender_t {
  explicit constexpr use_sender_t() = default;
};

struct use_awaitable_t {
  explicit constexpr use_awaitable_t() = default;
};

template <typename handler_t> struct use_callback_t {
  handler_t handler;
  std::stop_token stop_token{};
};

template <typename token_t>
concept callback_token = requires(token_t token) {
  token.handler;
  token.stop_token;
};

} // namespace wh::core
