// Defines internal safety helpers for guarded execution, exception mapping,
// and defensive conversions into result/error channels.
#pragma once

#include <concepts>
#include <exception>
#include <new>
#include <utility>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/type_traits.hpp"

namespace wh::internal {

/// Executes a callable and maps thrown exceptions to `result` error codes.
template <typename value_t, typename callable_t>
  requires wh::core::callable_with<callable_t>
[[nodiscard]] auto safe_call(callable_t &&callable,
                             const wh::core::errc fallback_error = wh::core::errc::internal_error)
    -> wh::core::result<value_t> {
  try {
    if constexpr (std::same_as<value_t, void>) {
      std::forward<callable_t>(callable)();
      return {};
    } else {
      return std::forward<callable_t>(callable)();
    }
  } catch (const std::bad_alloc &) {
    return wh::core::result<value_t>::failure(wh::core::errc::resource_exhausted);
  } catch (...) {
    return wh::core::result<value_t>::failure(fallback_error);
  }
}

} // namespace wh::internal
