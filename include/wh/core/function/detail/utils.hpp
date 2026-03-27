// Defines internal helper utilities shared by function wrapper internals,
// including type traits, forwarding, and invocation helpers.
#pragma once

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

#include "wh/core/type_traits.hpp"

namespace wh::core::fn_detail {

/// Type-dependent `false` utility for deferred static assertions.
template <typename type_t>
inline consteval auto make_false() -> bool {
  return false;
}

/// Sentinel type used only for computing member-pointer size.
class undefined_class;
inline constexpr std::size_t member_pointer_size =
    sizeof(void (undefined_class::*)());

/// Rounds `size` up to the requested alignment.
template <std::size_t alignment>
[[nodiscard]] inline consteval auto add_padding_to_size(std::size_t size)
    -> std::size_t {
  if (size == 0U) {
    return 0U;
  }
  return alignment * (((size - 1U) / alignment) + 1U);
}

/// Forwarding helper: keep scalars by value, forward non-scalars by rvalue-ref.
template <typename type_t>
using ref_non_trivials =
    std::conditional_t<std::is_scalar_v<type_t>, type_t, type_t &&>;

/// Selects invocability trait based on noexcept requirement.
template <typename type_t, typename return_t, bool is_noexcept,
          typename... param_types>
using is_invocable =
    std::conditional_t<is_noexcept, std::is_nothrow_invocable<type_t, param_types...>,
                       std::is_invocable<type_t, param_types...>>;

/// Detects pointer-like types exposing `pointer_traits::element_type`.
template <typename type_t>
inline constexpr bool is_dereferencable_v =
    requires { typename std::pointer_traits<type_t>::element_type; };

template <typename type_t>
using is_dereferencable = std::bool_constant<is_dereferencable_v<type_t>>;

/// Resolved pointee type of pointer-like wrapper.
template <typename type_t>
  requires is_dereferencable_v<type_t>
using dereferenced_t = typename std::pointer_traits<type_t>::element_type;

/// Detects plain function pointer types.
template <typename type_t>
inline constexpr bool is_function_pointer_v =
    std::is_function_v<typename std::remove_pointer_t<type_t>> &&
    std::is_pointer_v<type_t>;

template <typename type_t>
using is_function_pointer = std::bool_constant<is_function_pointer_v<type_t>>;

/// Detects `std::in_place_type_t<T>` marker types.
template <typename>
inline constexpr bool is_in_place_type_v = false;

template <typename type_t>
inline constexpr bool is_in_place_type_v<std::in_place_type_t<type_t>> = true;

template <typename type_t>
using is_in_place_type = std::bool_constant<is_in_place_type_v<type_t>>;

/// Removes rvalue-reference qualifier while keeping other qualifiers intact.
template <typename type_t>
using strip_rvalue_t =
    std::conditional_t<std::is_rvalue_reference_v<type_t>,
                       std::remove_reference_t<type_t>, type_t>;

/// Executes cleanup logic on scope exit unless disarmed.
template <typename cleanup_t>
class scope_guard {
private:
  cleanup_t cleanup_;
  bool active_;

public:
  explicit scope_guard(cleanup_t &&cleanup)
    requires(wh::core::callable_with<cleanup_t>)
      : cleanup_(std::forward<cleanup_t>(cleanup)), active_(true) {}

  ~scope_guard() {
    if (active_) {
      cleanup_();
    }
  }

  scope_guard(const scope_guard &) = delete;
  scope_guard &operator=(const scope_guard &) = delete;
  scope_guard(scope_guard &&) = delete;
  scope_guard &operator=(scope_guard &&) = delete;

  /// Disables cleanup execution on destruction.
  auto disarm() noexcept -> void { active_ = false; }
};

template <typename type_t>
scope_guard(type_t &&) -> scope_guard<strip_rvalue_t<type_t>>;

} // namespace wh::core::fn_detail
