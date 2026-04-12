// Defines reusable result-oriented sender helpers shared by component and
// compose async paths.
#pragma once

#include <concepts>
#include <utility>

#include <stdexec/execution.hpp>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"

namespace wh::core::detail {

template <typename result_t, stdexec::sender sender_t>
[[nodiscard]] constexpr auto normalize_result_sender(sender_t &&sender) {
  return std::forward<sender_t>(sender) |
         stdexec::upon_error([](auto &&) noexcept {
           return result_t::failure(wh::core::errc::internal_error);
         });
}

} // namespace wh::core::detail
