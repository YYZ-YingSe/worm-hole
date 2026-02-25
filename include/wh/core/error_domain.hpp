#pragma once

#include <exception>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"

namespace wh::core {

[[nodiscard]] inline auto map_exception(const std::exception &error) noexcept
    -> error_code {
  if (dynamic_cast<const std::bad_alloc *>(&error) != nullptr) {
    return make_error(errc::resource_exhausted);
  }
  if (dynamic_cast<const std::invalid_argument *>(&error) != nullptr ||
      dynamic_cast<const std::out_of_range *>(&error) != nullptr) {
    return make_error(errc::invalid_argument);
  }
  if (dynamic_cast<const std::logic_error *>(&error) != nullptr) {
    return make_error(errc::contract_violation);
  }
  if (dynamic_cast<const std::runtime_error *>(&error) != nullptr) {
    return make_error(errc::internal_error);
  }
  return make_error(errc::internal_error);
}

[[nodiscard]] inline auto map_current_exception() noexcept -> error_code {
  try {
    throw;
  } catch (const std::exception &error) {
    return map_exception(error);
  } catch (...) {
    return make_error(errc::internal_error);
  }
}

template <typename value_t, typename callable_t>
  requires std::invocable<callable_t>
[[nodiscard]] auto exception_boundary(callable_t &&callable)
    -> result<value_t> {
  try {
    if constexpr (std::same_as<value_t, void>) {
      std::forward<callable_t>(callable)();
      return {};
    } else {
      return std::forward<callable_t>(callable)();
    }
  } catch (...) {
    return result<value_t>::failure(map_current_exception());
  }
}

} // namespace wh::core
