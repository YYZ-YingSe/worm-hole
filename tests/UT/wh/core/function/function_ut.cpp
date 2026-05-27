#include <memory>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "wh/core/function/function.hpp"

namespace {

struct adder {
  int base{0};

  [[nodiscard]] auto operator()(int value) const -> int { return base + value; }
};

struct noexcept_adder {
  int base{0};

  [[nodiscard]] auto operator()(int value) const noexcept -> int { return base + value; }
};

[[nodiscard]] auto noexcept_free_adder(int value) noexcept -> int { return value + 9; }

struct mutable_adder {
  int base{0};

  [[nodiscard]] auto operator()(int value) const -> int { return base + value; }
};

struct address_probe_state {
  int copied{0};
  int destructed{0};
  int wrong_destructor_address{0};
};

struct self_address_callable {
  address_probe_state *state{};
  const void *self{};

  explicit self_address_callable(address_probe_state *next_state) : state(next_state), self(this) {}
  self_address_callable(const self_address_callable &other) : state(other.state), self(this) {
    ++state->copied;
  }
  self_address_callable(self_address_callable &&) noexcept = delete;
  auto operator=(const self_address_callable &) -> self_address_callable & = delete;
  auto operator=(self_address_callable &&) noexcept -> self_address_callable & = delete;

  ~self_address_callable() {
    ++state->destructed;
    if (self != this) {
      ++state->wrong_destructor_address;
    }
  }

  [[nodiscard]] auto operator()() const -> int { return 7; }
};

struct move_address_callable {
  address_probe_state *state{};
  const void *self{};

  explicit move_address_callable(address_probe_state *next_state) : state(next_state), self(this) {}
  move_address_callable(const move_address_callable &) = delete;
  auto operator=(const move_address_callable &) -> move_address_callable & = delete;
  move_address_callable(move_address_callable &&other) noexcept : state(other.state), self(this) {
    other.state = nullptr;
    other.self = nullptr;
  }
  auto operator=(move_address_callable &&) noexcept -> move_address_callable & = delete;

  ~move_address_callable() {
    if (state == nullptr) {
      return;
    }
    ++state->destructed;
    if (self != this) {
      ++state->wrong_destructor_address;
    }
  }

  [[nodiscard]] auto operator()() const -> int { return 11; }
};

} // namespace

TEST_CASE("standard_function supports callable construction swap nullptr "
          "assignment and throws when empty",
          "[UT][wh/core/function/"
          "function.hpp][standard_function][condition][branch][boundary]") {
  wh::core::standard_function<int(int)> add_one{[](int value) { return value + 1; }};
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

TEST_CASE("move_only_function and callback_function expose wrapper aliases and "
          "callable traits",
          "[UT][wh/core/function/"
          "function.hpp][move_only_function][branch][boundary]") {
  wh::core::move_only_function<int()> move_only{
      [payload = std::make_unique<int>(7)] { return *payload; }};
  REQUIRE(move_only() == 7);

  wh::core::callback_function<int(int)> callback{adder{4}};
  REQUIRE(callback(3) == 7);

  REQUIRE(wh::core::is_function_v<wh::core::standard_function<int(int)>>);
  REQUIRE(wh::core::is_function_v<decltype(callback)>);
  REQUIRE_FALSE(wh::core::is_function_v<int>);
}

TEST_CASE("function facade aligns copyability and const-callable construction rules",
          "[UT][wh/core/function/"
          "function.hpp][callback_function][condition][branch]") {
  const auto copyable = [base = 4](const int value) { return base + value; };
  using const_callback_t = wh::core::callback_function<int(int) const>;
  STATIC_REQUIRE(std::is_constructible_v<const_callback_t, decltype((copyable))>);

  const const_callback_t callback{copyable};
  REQUIRE(callback(3) == 7);

  auto lower = [payload = std::make_unique<int>(7)]() mutable { return *payload; };
  STATIC_REQUIRE(std::is_constructible_v<wh::core::move_only_function<int()>, decltype(lower)>);
  STATIC_REQUIRE(!std::is_constructible_v<wh::core::callback_function<int()>, decltype(lower)>);

  wh::core::move_only_function<int()> move_only{std::move(lower)};
  REQUIRE(move_only() == 7);
}

TEST_CASE("function facade supports noexcept-qualified signatures",
          "[UT][wh/core/function/function.hpp][function][noexcept][condition]") {
  using noexcept_function_t = wh::core::standard_function<int(int) noexcept>;
  STATIC_REQUIRE(std::is_constructible_v<noexcept_function_t, noexcept_adder>);
  STATIC_REQUIRE_FALSE(std::is_constructible_v<noexcept_function_t, adder>);

  noexcept_function_t function{noexcept_adder{5}};
  STATIC_REQUIRE(noexcept(function(1)));
  REQUIRE(function(2) == 7);

  using const_noexcept_callback_t = wh::core::callback_function<int(int) const noexcept>;
  STATIC_REQUIRE(std::is_constructible_v<const_noexcept_callback_t, noexcept_adder>);
  STATIC_REQUIRE_FALSE(std::is_constructible_v<const_noexcept_callback_t, adder>);

  const const_noexcept_callback_t callback{noexcept_adder{4}};
  STATIC_REQUIRE(noexcept(callback(1)));
  REQUIRE(callback(3) == 7);
}

TEST_CASE("function facade accepts noexcept function pointers through public policy queries",
          "[UT][wh/core/function/function.hpp][callback_function][noexcept][condition]") {
  using const_callback_t = wh::core::callback_function<int(int) const>;
  STATIC_REQUIRE(std::is_assignable_v<const_callback_t &, int (*)(int) noexcept>);

  const_callback_t callback{nullptr};
  callback = &noexcept_free_adder;
  REQUIRE(callback(2) == 11);
}

TEST_CASE("callback_function move relocates targets through legal construction",
          "[UT][wh/core/function/function.hpp][callback_function][move][lifetime]") {
  address_probe_state state{};

  {
    self_address_callable source_target{&state};
    wh::core::callback_function<int() const> source{source_target};
    REQUIRE(source() == 7);
    REQUIRE(state.copied == 1);

    auto moved = std::move(source);
    REQUIRE_FALSE(static_cast<bool>(source));
    REQUIRE(static_cast<bool>(moved));
    REQUIRE(moved() == 7);
    REQUIRE(state.copied == 1);

    auto copied_after_move = moved;
    REQUIRE(copied_after_move() == 7);
    REQUIRE(state.copied == 2);

    wh::core::callback_function<int() const> assigned{nullptr};
    assigned = std::move(moved);
    REQUIRE_FALSE(static_cast<bool>(moved));
    REQUIRE(static_cast<bool>(assigned));
    REQUIRE(assigned() == 7);
    REQUIRE(state.copied == 2);
  }

  REQUIRE(state.destructed >= 1);
  REQUIRE(state.wrong_destructor_address == 0);
}

TEST_CASE("move_only_function move constructs inline targets at their new address",
          "[UT][wh/core/function/function.hpp][move_only_function][move][lifetime]") {
  address_probe_state state{};

  {
    wh::core::move_only_function<int()> source{move_address_callable{&state}};
    REQUIRE(source() == 11);

    auto moved = std::move(source);
    REQUIRE_FALSE(static_cast<bool>(source));
    REQUIRE(static_cast<bool>(moved));
    REQUIRE(moved() == 11);

    wh::core::move_only_function<int()> assigned{nullptr};
    assigned = std::move(moved);
    REQUIRE_FALSE(static_cast<bool>(moved));
    REQUIRE(static_cast<bool>(assigned));
    REQUIRE(assigned() == 11);
  }

  REQUIRE(state.destructed >= 1);
  REQUIRE(state.wrong_destructor_address == 0);
}
