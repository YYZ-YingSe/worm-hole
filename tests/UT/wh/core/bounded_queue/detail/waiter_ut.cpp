#include <exception>
#include <memory>
#include <stdexcept>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "wh/core/bounded_queue/detail/waiter.hpp"

namespace {

struct tracked_waiter_value {
  static inline int live_count = 0;

  int value{0};

  tracked_waiter_value() { ++live_count; }
  explicit tracked_waiter_value(int input) : value(input) { ++live_count; }
  tracked_waiter_value(const tracked_waiter_value &other) : value(other.value) { ++live_count; }
  tracked_waiter_value(tracked_waiter_value &&other) noexcept : value(other.value) { ++live_count; }
  ~tracked_waiter_value() { --live_count; }
};

} // namespace

TEST_CASE("push waiter base tracks source success closed error and stopped states",
          "[UT][wh/core/bounded_queue/detail/"
          "waiter.hpp][push_waiter_base::store_error][condition][branch]") {
  wh::core::detail::push_waiter_base<std::string> waiter{};
  std::string source = "value";

  waiter.set_copy_source(source);
  REQUIRE(waiter.has_copy_source());
  REQUIRE(waiter.source_value() == "value");

  waiter.set_move_source(source);
  REQUIRE(waiter.has_move_source());
  REQUIRE(waiter.movable_source_value() == "value");

  waiter.store_success();
  REQUIRE(waiter.is_success());
  waiter.store_closed();
  REQUIRE(waiter.is_closed());
  waiter.store_stopped();
  REQUIRE(waiter.is_stopped());

  auto first_error = std::make_exception_ptr(std::runtime_error{"first"});
  waiter.store_error(first_error);
  REQUIRE(waiter.is_error());
  REQUIRE(waiter.error() != nullptr);

  auto second_error = std::make_exception_ptr(std::logic_error{"second"});
  waiter.store_error(second_error);
  REQUIRE(waiter.is_error());
  REQUIRE(waiter.error() != nullptr);
}

TEST_CASE("pop waiter base owns values and tears them down across terminal transitions",
          "[UT][wh/core/bounded_queue/detail/"
          "waiter.hpp][pop_waiter_base::emplace_value][condition][branch][boundary]") {
  tracked_waiter_value::live_count = 0;

  wh::core::detail::pop_waiter_base<tracked_waiter_value> waiter{};
  waiter.emplace_value(7);
  REQUIRE(waiter.has_value());
  REQUIRE(waiter.value().value == 7);
  REQUIRE(tracked_waiter_value::live_count == 1);

  auto error = std::make_exception_ptr(std::runtime_error{"boom"});
  waiter.store_error(error);
  REQUIRE(waiter.is_error());
  REQUIRE(waiter.error() != nullptr);
  REQUIRE(tracked_waiter_value::live_count == 0);

  waiter.store_closed();
  REQUIRE(waiter.is_closed());
  waiter.store_stopped();
  REQUIRE(waiter.is_stopped());
}
