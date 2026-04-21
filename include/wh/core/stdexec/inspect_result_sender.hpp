// Defines pass-through inspection for sender<result<T>> pipelines.
#pragma once

#include <concepts>
#include <functional>
#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

#include "wh/core/result.hpp"

namespace wh::core::detail {

template <stdexec::sender sender_t, typename inspector_t>
[[nodiscard]] constexpr auto inspect_result_sender(sender_t &&sender, inspector_t &&inspector) {
  return std::forward<sender_t>(sender) |
         stdexec::then([inspector = std::forward<inspector_t>(inspector)](auto status) mutable {
           using status_t = std::remove_cvref_t<decltype(status)>;
           static_assert(result_like<status_t>,
                         "inspect_result_sender requires sender<result<T>> input");
           std::invoke(inspector, status);
           return std::move(status);
         });
}

} // namespace wh::core::detail
