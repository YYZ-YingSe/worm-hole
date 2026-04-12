#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include "wh/core/function/detail/utils.hpp"

namespace {

inline constexpr auto plus_one_ptr = +[](int value) -> int {
  return value + 1;
};

static_assert(!wh::core::fn_detail::make_false<int>());
static_assert(wh::core::fn_detail::member_pointer_size > 0U);
static_assert(wh::core::fn_detail::add_padding_to_size<8U>(0U) == 0U);
static_assert(wh::core::fn_detail::add_padding_to_size<8U>(1U) == 8U);
static_assert(wh::core::fn_detail::add_padding_to_size<8U>(9U) == 16U);
static_assert(std::same_as<wh::core::fn_detail::ref_non_trivials<int>, int>);
static_assert(std::same_as<wh::core::fn_detail::strip_rvalue_t<std::string &&>,
                           std::string>);
static_assert(wh::core::fn_detail::is_dereferencable_v<std::unique_ptr<int>>);
static_assert(!wh::core::fn_detail::is_dereferencable_v<int>);
static_assert(std::same_as<
              wh::core::fn_detail::dereferenced_t<std::unique_ptr<int>>, int>);
static_assert(wh::core::fn_detail::is_function_pointer_v<decltype(plus_one_ptr)>);
static_assert(wh::core::fn_detail::is_in_place_type_v<std::in_place_type_t<int>>);
static_assert(!wh::core::fn_detail::is_in_place_type_v<int>);
static_assert(
    wh::core::fn_detail::is_invocable<decltype(plus_one_ptr), int, false,
                                      int>::value);

} // namespace

TEST_CASE("function detail scope_guard runs cleanup unless disarmed",
          "[UT][wh/core/function/detail/utils.hpp][scope_guard][branch]") {
  bool fired = false;
  {
    wh::core::fn_detail::scope_guard guard{[&]() { fired = true; }};
  }
  REQUIRE(fired);

  fired = false;
  {
    wh::core::fn_detail::scope_guard guard{[&]() { fired = true; }};
    guard.disarm();
  }
  REQUIRE_FALSE(fired);
}

TEST_CASE("function detail size and forwarding helpers preserve compile-time layout rules",
          "[UT][wh/core/function/detail/utils.hpp][add_padding_to_size][condition][boundary]") {
  STATIC_REQUIRE(wh::core::fn_detail::member_pointer_size > 0U);
  REQUIRE(wh::core::fn_detail::add_padding_to_size<8U>(0U) == 0U);
  REQUIRE(wh::core::fn_detail::add_padding_to_size<8U>(1U) == 8U);
  REQUIRE(wh::core::fn_detail::add_padding_to_size<8U>(8U) == 8U);
  REQUIRE(wh::core::fn_detail::add_padding_to_size<8U>(9U) == 16U);
  REQUIRE(wh::core::fn_detail::add_padding_to_size<4U>(5U) == 8U);

  STATIC_REQUIRE(
      std::same_as<wh::core::fn_detail::ref_non_trivials<int>, int>);
  STATIC_REQUIRE(std::same_as<
                 wh::core::fn_detail::ref_non_trivials<std::string>,
                 std::string &&>);
  STATIC_REQUIRE(std::same_as<
                 wh::core::fn_detail::strip_rvalue_t<std::string &&>,
                 std::string>);
  STATIC_REQUIRE(std::same_as<
                 wh::core::fn_detail::strip_rvalue_t<const std::string &>,
                 const std::string &>);
}

TEST_CASE("function detail traits classify pointer-like function-pointer and in-place markers",
          "[UT][wh/core/function/detail/utils.hpp][is_dereferencable_v][condition][branch]") {
  STATIC_REQUIRE(!wh::core::fn_detail::make_false<int>());
  STATIC_REQUIRE(wh::core::fn_detail::is_dereferencable_v<std::unique_ptr<int>>);
  STATIC_REQUIRE(!wh::core::fn_detail::is_dereferencable_v<int>);
  STATIC_REQUIRE(std::same_as<
                 wh::core::fn_detail::dereferenced_t<std::unique_ptr<int>>,
                 int>);
  STATIC_REQUIRE(
      wh::core::fn_detail::is_function_pointer_v<decltype(plus_one_ptr)>);
  STATIC_REQUIRE(!wh::core::fn_detail::is_function_pointer_v<int>);
  STATIC_REQUIRE(
      wh::core::fn_detail::is_in_place_type_v<std::in_place_type_t<int>>);
  STATIC_REQUIRE(!wh::core::fn_detail::is_in_place_type_v<int>);
  STATIC_REQUIRE(
      wh::core::fn_detail::is_invocable<decltype(plus_one_ptr), int, false,
                                        int>::value);
}
