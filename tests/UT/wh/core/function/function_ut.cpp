#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "wh/core/function/function.hpp"

namespace {

struct adder {
  int base{0};

  [[nodiscard]] auto operator()(int value) const -> int { return base + value; }
};

struct mutable_adder {
  int base{0};

  [[nodiscard]] auto operator()(int value) const -> int { return base + value; }
};

} // namespace

TEST_CASE("standard_function supports callable construction swap nullptr assignment and throws when empty",
          "[UT][wh/core/function/function.hpp][standard_function][condition][branch][boundary]") {
  wh::core::standard_function<int(int)> add_one{[](int value) {
    return value + 1;
  }};
  REQUIRE(add_one(3) == 4);

  wh::core::standard_function<int(int)> add_two{adder{2}};
  swap(add_one, add_two);
  REQUIRE(add_one(3) == 5);

  add_two = nullptr;
  REQUIRE(add_two == nullptr);
  REQUIRE_THROWS_AS(add_two(1), std::bad_function_call);
}

TEST_CASE("function_ref observes external callable state without taking ownership",
          "[UT][wh/core/function/function.hpp][function_ref][branch][boundary]") {
  mutable_adder functor{.base = 3};
  wh::core::function_ref<int(int)> borrowed{functor};
  REQUIRE(borrowed(4) == 7);

  functor.base = 10;
  REQUIRE(borrowed(1) == 11);
}

TEST_CASE("move_only_function and callback_function expose wrapper aliases and callable traits",
          "[UT][wh/core/function/function.hpp][move_only_function][branch][boundary]") {
  wh::core::move_only_function<int()> move_only{
      [payload = std::make_unique<int>(7)] { return *payload; }};
  REQUIRE(move_only() == 7);

  wh::core::callback_function<int(int)> callback{adder{4}};
  REQUIRE(callback(3) == 7);

  REQUIRE(wh::core::is_function_v<wh::core::standard_function<int(int)>>);
  REQUIRE(wh::core::is_function_v<decltype(callback)>);
  REQUIRE_FALSE(wh::core::is_function_v<int>);
}
