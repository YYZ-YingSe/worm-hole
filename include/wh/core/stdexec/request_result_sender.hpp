// Defines request-value-category aware sender dispatch for sender<result<T>>
// component entry helpers.
#pragma once

#include <concepts>
#include <functional>
#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

#include "wh/core/result.hpp"
#include "wh/core/stdexec/defer_sender.hpp"
#include "wh/core/stdexec/result_sender.hpp"

namespace wh::core::detail {

template <typename result_t, typename request_t, typename call_t>
  requires result_like<result_t>
[[nodiscard]] constexpr auto request_result_sender(request_t &&request,
                                                   call_t &&call) {
  using request_value_t = std::remove_cvref_t<request_t>;

  if constexpr (std::is_lvalue_reference_v<request_t> ||
                std::is_const_v<std::remove_reference_t<request_t>>) {
    if constexpr (std::invocable<call_t, request_t>) {
      return normalize_result_sender<result_t>(std::invoke(
          std::forward<call_t>(call), std::forward<request_t>(request)));
    } else {
      static_assert(
          std::invocable<call_t, request_value_t>,
          "request_result_sender requires callable request sender factory");
      return normalize_result_sender<result_t>(
          std::invoke(std::forward<call_t>(call), request_value_t{request}));
    }
  } else {
    if constexpr (std::invocable<call_t, request_t>) {
      return normalize_result_sender<result_t>(std::invoke(
          std::forward<call_t>(call), std::forward<request_t>(request)));
    } else {
      return request_result_sender<result_t>(
          static_cast<const request_value_t &>(request),
          std::forward<call_t>(call));
    }
  }
}

template <typename result_t, typename request_t, typename call_t>
  requires result_like<result_t>
[[nodiscard]] constexpr auto defer_request_result_sender(request_t &&request,
                                                         call_t &&call) {
  using stored_request_t = std::remove_cvref_t<request_t>;
  using stored_call_t = std::remove_cvref_t<call_t>;

  return defer_result_sender<result_t>(
      [request = stored_request_t{std::forward<request_t>(request)},
       call = stored_call_t{std::forward<call_t>(call)}]() mutable {
        return request_result_sender<result_t>(std::move(request),
                                               std::move(call));
      });
}

} // namespace wh::core::detail
