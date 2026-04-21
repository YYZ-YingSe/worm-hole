#include <memory>
#include <type_traits>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "wh/core/function.hpp"

namespace {

struct adder {
  int base{0};

  [[nodiscard]] auto operator()(const int value) const -> int { return base + value; }
};

using wh::core::callback_function;
using wh::core::function_ref;
using wh::core::move_only_function;
using wh::core::standard_function;

static_assert(wh::core::is_function_v<standard_function<int(int)>>);
static_assert(wh::core::is_function<function_ref<int(int)>>::value);
static_assert(
    std::same_as<
        wh::core::owning_storage<int(int), wh::core::reference_counting, wh::core::standard_accept,
                                 wh::core::assert_on_error, sizeof(void *), std::allocator>,
        wh::core::owning_storage<int(int), wh::core::reference_counting, wh::core::standard_accept,
                                 wh::core::assert_on_error, sizeof(void *), std::allocator>>);

} // namespace

TEST_CASE("standard_function constructs invokes swaps and compares with nullptr",
          "[UT][wh/core/function.hpp][standard_function][branch][boundary]") {
  standard_function<int(int)> add_one{[](int value) { return value + 1; }};
  standard_function<int(int)> add_two{adder{2}};

  REQUIRE(static_cast<bool>(add_one));
  REQUIRE(add_one(3) == 4);
  REQUIRE(add_two(3) == 5);
  REQUIRE_FALSE(add_one == nullptr);

  swap(add_one, add_two);
  REQUIRE(add_one(3) == 5);
  REQUIRE(add_two(3) == 4);

  add_two = nullptr;
  REQUIRE(add_two == nullptr);
}

TEST_CASE("function_ref borrows callable state without taking ownership",
          "[UT][wh/core/function.hpp][function_ref][branch]") {
  int multiplier = 3;
  auto callable = [&multiplier](const int value) { return value * multiplier; };

  function_ref<int(int)> ref{callable};
  REQUIRE(ref(4) == 12);

  multiplier = 5;
  REQUIRE(ref(4) == 20);
}

TEST_CASE("move_only_function and callback_function wrap callable categories",
          "[UT][wh/core/function.hpp][move_only_function][branch]") {
  move_only_function<int()> move_only{[payload = std::make_unique<int>(7)]() { return *payload; }};
  REQUIRE(move_only() == 7);

  callback_function<void(int)> callback{[&](int value) { REQUIRE(value == 9); }};
  callback(9);
}

TEST_CASE("function facade aliases preserve empty-wrapper semantics",
          "[UT][wh/core/function.hpp][is_function_v][condition][boundary]") {
  standard_function<int(int)> standard{nullptr};
  move_only_function<int()> move_only{nullptr};
  callback_function<void()> callback{nullptr};

  REQUIRE_FALSE(static_cast<bool>(standard));
  REQUIRE_FALSE(static_cast<bool>(move_only));
  REQUIRE_FALSE(static_cast<bool>(callback));
  REQUIRE(standard == nullptr);
  REQUIRE(move_only == nullptr);
  REQUIRE(callback == nullptr);

  STATIC_REQUIRE(wh::core::is_function_v<standard_function<int(int)>>);
  STATIC_REQUIRE(wh::core::is_function_v<move_only_function<int()>>);
  STATIC_REQUIRE(wh::core::is_function_v<callback_function<void()>>);
}
