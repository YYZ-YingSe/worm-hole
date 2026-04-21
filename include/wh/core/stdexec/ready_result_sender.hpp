// Defines ready stdexec senders for immediate values and result failures.
#pragma once

#include <concepts>
#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

#include "wh/core/result.hpp"

namespace wh::core::detail {

template <typename value_t> using ready_sender_t = decltype(stdexec::just(std::declval<value_t>()));

template <typename value_t> [[nodiscard]] constexpr auto ready_sender(value_t &&value) {
  return stdexec::just(std::forward<value_t>(value));
}

template <typename result_t, typename error_t>
  requires result_like<result_t>
using failure_result_sender_t =
    ready_sender_t<decltype(result_t::failure(std::declval<error_t>()))>;

template <typename result_t, typename error_t>
  requires result_like<result_t>
[[nodiscard]] constexpr auto failure_result_sender(error_t &&error) {
  return ready_sender(result_t::failure(std::forward<error_t>(error)));
}

} // namespace wh::core::detail
