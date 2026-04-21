#include <functional>
#include <stdexcept>

#include <catch2/catch_test_macros.hpp>

#include "wh/core/function/detail/error_policy.hpp"

namespace {

struct check_none_probe : wh::core::fn::check_none {
  static constexpr bool before_access = check_before_access;
  static constexpr bool before_call = check_before_call;

  static auto invoke() noexcept -> void { on_invoke(); }
};

struct check_none_base_probe : wh::core::fn_detail::check_none_base {
  static constexpr bool before_access = check_before_access;
  static constexpr bool before_copy = check_before_copy;
  static constexpr bool before_destroy = check_before_destroy;

  static auto access() noexcept -> void { on_access(); }
  static auto copy() noexcept -> void { on_copy(); }
  static auto destroy() noexcept -> void { on_destroy(); }
};

struct skip_probe : wh::core::fn::skip_on_error {
  static constexpr bool before_access = check_before_access;
  static constexpr bool before_call = check_before_call;

  static auto invoke() noexcept -> void { on_invoke(); }
};

struct skip_base_probe : wh::core::fn_detail::skip_on_error_base {
  static constexpr bool before_access = check_before_access;
  static constexpr bool before_copy = check_before_copy;
  static constexpr bool before_destroy = check_before_destroy;

  static auto access() noexcept -> void { on_access(); }
  static auto copy() noexcept -> void { on_copy(); }
  static auto destroy() noexcept -> void { on_destroy(); }
};

struct assert_probe : wh::core::fn::assert_on_error {
  static constexpr bool before_access = check_before_access;
  static constexpr bool before_call = check_before_call;
};

struct throw_probe : wh::core::fn::throw_on_error {
  static constexpr bool before_access = check_before_access;
  static constexpr bool before_call = check_before_call;

  static auto invoke() -> void { on_invoke(); }
};

struct throw_base_probe : wh::core::fn_detail::throw_on_error_base {
  static auto access() -> void { on_access(); }
  static auto copy() -> void { on_copy(); }
};

} // namespace

TEST_CASE("error policies expose check flags for function wrapper behavior",
          "[UT][wh/core/function/detail/error_policy.hpp][check_none][condition][branch]") {
  REQUIRE_FALSE(check_none_base_probe::before_access);
  REQUIRE_FALSE(check_none_base_probe::before_copy);
  REQUIRE_FALSE(check_none_base_probe::before_destroy);
  REQUIRE_FALSE(check_none_probe::before_access);
  REQUIRE_FALSE(check_none_probe::before_call);
  REQUIRE(skip_base_probe::before_access);
  REQUIRE(skip_base_probe::before_copy);
  REQUIRE(skip_base_probe::before_destroy);
  REQUIRE_FALSE(skip_probe::before_access);
  REQUIRE_FALSE(skip_probe::before_call);
  REQUIRE(assert_probe::before_access);
  REQUIRE(assert_probe::before_call);
  REQUIRE(throw_probe::before_access);
  REQUIRE(throw_probe::before_call);
}

TEST_CASE("non-throwing error policies keep invalid-operation hooks as no-ops",
          "[UT][wh/core/function/detail/error_policy.hpp][skip_on_error][branch][boundary]") {
  REQUIRE_NOTHROW(check_none_base_probe::access());
  REQUIRE_NOTHROW(check_none_base_probe::copy());
  REQUIRE_NOTHROW(check_none_base_probe::destroy());
  REQUIRE_NOTHROW(skip_base_probe::access());
  REQUIRE_NOTHROW(skip_base_probe::copy());
  REQUIRE_NOTHROW(skip_base_probe::destroy());
  REQUIRE_NOTHROW(check_none_probe::invoke());
  REQUIRE_NOTHROW(skip_probe::invoke());
}

TEST_CASE("throwing error policies report invalid operations through exceptions",
          "[UT][wh/core/function/detail/error_policy.hpp][throw_on_error][branch]") {
  REQUIRE_THROWS_AS(throw_probe::invoke(), std::bad_function_call);
  REQUIRE_THROWS_AS(throw_base_probe::access(), std::logic_error);
  REQUIRE_THROWS_AS(throw_base_probe::copy(), std::logic_error);
}
