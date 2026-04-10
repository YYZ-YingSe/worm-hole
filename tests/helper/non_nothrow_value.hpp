// Probe value type used to verify stream/queue support for values whose copy
// and move operations are available but not marked noexcept.
#pragma once

#include <concepts>
#include <type_traits>

namespace wh::testing::helper {

struct non_nothrow_value {
  int value{0};

  non_nothrow_value() = default;
  explicit non_nothrow_value(const int input) noexcept : value(input) {}

  non_nothrow_value(const non_nothrow_value &other) noexcept(false)
      : value(other.value) {}
  non_nothrow_value(non_nothrow_value &&other) noexcept(false)
      : value(other.value) {}

  auto operator=(const non_nothrow_value &) -> non_nothrow_value & = default;
  auto operator=(non_nothrow_value &&) -> non_nothrow_value & = default;

  [[nodiscard]] friend auto operator==(const non_nothrow_value &lhs,
                                       const non_nothrow_value &rhs) noexcept
      -> bool {
    return lhs.value == rhs.value;
  }
};

static_assert(std::copy_constructible<non_nothrow_value>);
static_assert(std::move_constructible<non_nothrow_value>);
static_assert(!std::is_nothrow_copy_constructible_v<non_nothrow_value>);
static_assert(!std::is_nothrow_move_constructible_v<non_nothrow_value>);

} // namespace wh::testing::helper
