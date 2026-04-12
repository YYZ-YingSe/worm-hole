#include <catch2/catch_test_macros.hpp>

#include <utility>

#include "wh/core/stdexec/manual_lifetime_box.hpp"

namespace {

struct tracked_value {
  static inline int live_count = 0;
  static inline int destroy_count = 0;

  int value{0};

  tracked_value() { ++live_count; }
  explicit tracked_value(int input) : value(input) { ++live_count; }
  tracked_value(const tracked_value &other) : value(other.value) { ++live_count; }
  tracked_value(tracked_value &&other) noexcept : value(other.value) { ++live_count; }
  ~tracked_value() {
    --live_count;
    ++destroy_count;
  }
};

} // namespace

TEST_CASE("manual lifetime box emplace emplace_from reset and destructor manage lifetime",
          "[UT][wh/core/stdexec/manual_lifetime_box.hpp][manual_lifetime_box::emplace][condition][branch][boundary]") {
  tracked_value::live_count = 0;
  tracked_value::destroy_count = 0;

  {
    wh::core::detail::manual_lifetime_box<tracked_value> box{};
    REQUIRE_FALSE(box.has_value());

    auto &first = box.emplace(7);
    REQUIRE(box.has_value());
    REQUIRE(first.value == 7);
    REQUIRE(box.get().value == 7);
    REQUIRE(tracked_value::live_count == 1);

    auto &second = box.emplace_from(
        [](int value) { return tracked_value{value + 1}; }, 8);
    REQUIRE(second.value == 9);
    REQUIRE(box.get().value == 9);
    REQUIRE(tracked_value::live_count == 1);
    REQUIRE(tracked_value::destroy_count >= 1);

    box.reset();
    REQUIRE_FALSE(box.has_value());
    REQUIRE(tracked_value::live_count == 0);

    box.reset();
    REQUIRE(tracked_value::live_count == 0);
  }

  REQUIRE(tracked_value::live_count == 0);
}

TEST_CASE("manual lifetime box replaces existing value and preserves latest payload",
          "[UT][wh/core/stdexec/manual_lifetime_box.hpp][manual_lifetime_box::reset][condition][branch][boundary]") {
  tracked_value::live_count = 0;
  tracked_value::destroy_count = 0;

  wh::core::detail::manual_lifetime_box<tracked_value> box{};
  box.emplace(1);
  REQUIRE(box.has_value());
  REQUIRE(box.get().value == 1);

  box.emplace(5);
  REQUIRE(box.has_value());
  REQUIRE(box.get().value == 5);
  REQUIRE(tracked_value::live_count == 1);
  REQUIRE(tracked_value::destroy_count >= 1);

  box.reset();
  REQUIRE_FALSE(box.has_value());
  REQUIRE(tracked_value::live_count == 0);
}
