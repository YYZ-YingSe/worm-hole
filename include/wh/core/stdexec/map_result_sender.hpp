// Defines success-only result mapping for sender<result<T>> pipelines.
#pragma once

#include <concepts>
#include <functional>
#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

#include "wh/core/result.hpp"

namespace wh::core::detail {

template <typename next_result_t, typename mapped_t>
  requires result_like<next_result_t>
[[nodiscard]] constexpr auto map_result_value(mapped_t &&mapped)
    -> next_result_t {
  using mapped_value_t = std::remove_cvref_t<mapped_t>;
  if constexpr (result_like<mapped_value_t>) {
    static_assert(std::constructible_from<next_result_t, mapped_t>,
                  "map_result_sender requires mapper result to construct the "
                  "target result");
    return next_result_t{std::forward<mapped_t>(mapped)};
  } else {
    static_assert(std::constructible_from<next_result_t, mapped_t>,
                  "map_result_sender requires mapper value to construct the "
                  "target result");
    return next_result_t{std::forward<mapped_t>(mapped)};
  }
}

template <typename next_result_t, typename mapper_t, typename error_t>
  requires result_like<next_result_t>
[[nodiscard]] constexpr auto
map_result_status(mapper_t &&mapper, wh::core::result<void, error_t> status)
    -> next_result_t {
  static_assert(
      std::convertible_to<error_t, typename next_result_t::error_type>,
      "map_result_sender requires compatible result error types");
  if (status.has_error()) {
    return next_result_t::failure(std::move(status).error());
  }

  static_assert(std::invocable<mapper_t>,
                "map_result_sender requires a zero-argument mapper for "
                "result<void> input");
  using mapped_t = std::invoke_result_t<mapper_t>;
  if constexpr (std::same_as<mapped_t, void>) {
    static_assert(std::same_as<typename next_result_t::value_type, void>,
                  "map_result_sender requires result<void> target when mapper "
                  "returns void");
    std::invoke(std::forward<mapper_t>(mapper));
    return next_result_t{};
  } else {
    return map_result_value<next_result_t>(
        std::invoke(std::forward<mapper_t>(mapper)));
  }
}

template <typename next_result_t, typename mapper_t, typename value_t,
          typename error_t>
  requires result_like<next_result_t>
[[nodiscard]] constexpr auto
map_result_status(mapper_t &&mapper, wh::core::result<value_t, error_t> status)
    -> next_result_t {
  static_assert(
      std::convertible_to<error_t, typename next_result_t::error_type>,
      "map_result_sender requires compatible result error types");
  if (status.has_error()) {
    return next_result_t::failure(std::move(status).error());
  }

  static_assert(std::invocable<mapper_t, value_t &&>,
                "map_result_sender requires mapper(value) for result<T> input");
  using mapped_t = std::invoke_result_t<mapper_t, value_t &&>;
  if constexpr (std::same_as<mapped_t, void>) {
    static_assert(std::same_as<typename next_result_t::value_type, void>,
                  "map_result_sender requires result<void> target when mapper "
                  "returns void");
    std::invoke(std::forward<mapper_t>(mapper), std::move(status).value());
    return next_result_t{};
  } else {
    return map_result_value<next_result_t>(
        std::invoke(std::forward<mapper_t>(mapper), std::move(status).value()));
  }
}

template <typename next_result_t, stdexec::sender sender_t, typename mapper_t>
  requires result_like<next_result_t>
[[nodiscard]] constexpr auto map_result_sender(sender_t &&sender,
                                               mapper_t &&mapper) {
  return std::forward<sender_t>(sender) |
         stdexec::then([mapper = std::forward<mapper_t>(mapper)](
                           auto status) mutable -> next_result_t {
           using status_t = std::remove_cvref_t<decltype(status)>;
           static_assert(result_like<status_t>,
                         "map_result_sender requires sender<result<T>> input");
           return map_result_status<next_result_t>(std::move(mapper),
                                                   std::move(status));
         });
}

} // namespace wh::core::detail
