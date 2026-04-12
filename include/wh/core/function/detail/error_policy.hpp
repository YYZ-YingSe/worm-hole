// Defines error policies used by function wrappers to handle invalid calls,
// empty-state invocation, and contract failures.
#pragma once

#include <cassert>
#include <functional>
#include <stdexcept>

namespace wh::core::fn_detail {

/// Disables all validity checks in function/object manager internals.
class check_none_base {
protected:
  /// Do not validate access on read paths.
  static constexpr bool check_before_access = false;
  /// Do not validate source validity before copy paths.
  static constexpr bool check_before_copy = false;
  /// Do not validate state before destroy paths.
  static constexpr bool check_before_destroy = false;

  static auto on_access() noexcept -> void {}
  static auto on_copy() noexcept -> void {}
  static auto on_destroy() noexcept -> void {}

  check_none_base() = default;
  ~check_none_base() = default;
};

/// Enables checks but turns failures into no-op handlers.
class skip_on_error_base {
protected:
  /// Validate access calls and route failures to `on_access`.
  static constexpr bool check_before_access = true;
  /// Validate copy calls and route failures to `on_copy`.
  static constexpr bool check_before_copy = true;
  /// Validate destroy calls and route failures to `on_destroy`.
  static constexpr bool check_before_destroy = true;

  static auto on_access() noexcept -> void {}
  static auto on_copy() noexcept -> void {}
  static auto on_destroy() noexcept -> void {}

  skip_on_error_base() = default;
  ~skip_on_error_base() = default;
};

/// Enables checks and fails fast by assertion on invalid operations.
class assert_on_error_base : protected skip_on_error_base {
protected:
  static auto on_access() noexcept -> void { assert(false); }
  static auto on_copy() noexcept -> void { assert(false); }

  assert_on_error_base() = default;
  ~assert_on_error_base() = default;
};

/// Enables checks and reports invalid operations through exceptions.
class throw_on_error_base : protected skip_on_error_base {
protected:
  static auto on_access() -> void {
    throw std::logic_error("Attempted to access an invalid object!");
  }
  static auto on_copy() -> void {
    throw std::logic_error("Attempted to copy an invalid object!");
  }

  throw_on_error_base() = default;
  ~throw_on_error_base() = default;
};

} // namespace wh::core::fn_detail

namespace wh::core::fn {

/// Public policy: skip all runtime checks for maximum throughput.
struct check_none : protected fn_detail::check_none_base {
protected:
  /// Do not check validity before callable access.
  static constexpr bool check_before_access = false;
  /// Do not check validity before invoke.
  static constexpr bool check_before_call = false;

  static auto on_invoke() noexcept -> void {}

  check_none() = default;
  ~check_none() = default;
};

/// Public policy: check state but silently skip invalid operations.
struct skip_on_error : protected fn_detail::skip_on_error_base {
protected:
  /// Invoke path intentionally skips validity checks.
  static constexpr bool check_before_access = false;
  /// Invoke path intentionally skips validity checks.
  static constexpr bool check_before_call = false;

  static auto on_invoke() noexcept -> void {}

  skip_on_error() = default;
  ~skip_on_error() = default;
};

/// Public policy: assert when invoking/accessing invalid state.
struct assert_on_error : protected fn_detail::assert_on_error_base {
protected:
  /// Guard callable access.
  static constexpr bool check_before_access = true;
  /// Guard callable invocation.
  static constexpr bool check_before_call = true;

  static auto on_invoke() noexcept -> void { assert(false); }

  assert_on_error() = default;
  ~assert_on_error() = default;
};

/// Public policy: throw standard exceptions on invalid invocations.
struct throw_on_error : protected fn_detail::throw_on_error_base {
protected:
  /// Guard callable access.
  static constexpr bool check_before_access = true;
  /// Guard callable invocation.
  static constexpr bool check_before_call = true;

  static auto on_invoke() -> void { throw std::bad_function_call(); }

  throw_on_error() = default;
  ~throw_on_error() = default;
};

} // namespace wh::core::fn
