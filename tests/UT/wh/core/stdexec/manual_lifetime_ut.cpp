#include <catch2/catch_test_macros.hpp>

#include <utility>

#include "wh/core/stdexec/manual_lifetime.hpp"

namespace {

struct tracked_value {
  static inline int constructed = 0;
  static inline int destroyed = 0;

  int value{0};

  explicit tracked_value(const int next) noexcept : value(next) {
    ++constructed;
  }

  tracked_value(const tracked_value &) = delete;
  auto operator=(const tracked_value &) -> tracked_value & = delete;

  tracked_value(tracked_value &&other) noexcept : value(other.value) {
    ++constructed;
  }

  ~tracked_value() noexcept { ++destroyed; }
};

} // namespace

TEST_CASE("manual_lifetime constructs and destructs values explicitly",
          "[UT][wh/core/stdexec/manual_lifetime.hpp][manual_lifetime::construct][manual_lifetime::destruct]") {
  tracked_value::constructed = 0;
  tracked_value::destroyed = 0;

  wh::core::detail::manual_lifetime<tracked_value> storage{};
  auto &value = storage.construct(7);
  REQUIRE(value.value == 7);
  REQUIRE(tracked_value::constructed == 1);
  REQUIRE(tracked_value::destroyed == 0);

  storage.destruct();
  REQUIRE(tracked_value::destroyed == 1);
}

TEST_CASE("manual_storage constructs and destructs typed values explicitly",
          "[UT][wh/core/stdexec/manual_lifetime.hpp][manual_storage::construct][manual_storage::destruct]") {
  tracked_value::constructed = 0;
  tracked_value::destroyed = 0;

  wh::core::detail::manual_storage<sizeof(tracked_value), alignof(tracked_value)>
      storage{};
  auto &value = storage.construct<tracked_value>(29);
  REQUIRE(value.value == 29);
  REQUIRE(tracked_value::constructed == 1);
  REQUIRE(tracked_value::destroyed == 0);

  storage.destruct<tracked_value>();
  REQUIRE(tracked_value::destroyed == 1);
}

TEST_CASE("manual_lifetime construct_with preserves exact value type",
          "[UT][wh/core/stdexec/manual_lifetime.hpp][manual_lifetime::construct_with][branch]") {
  tracked_value::constructed = 0;
  tracked_value::destroyed = 0;

  wh::core::detail::manual_lifetime<tracked_value> storage{};
  auto &value = storage.construct_with([]() noexcept { return tracked_value{11}; });
  REQUIRE(value.value == 11);
  REQUIRE(tracked_value::constructed >= 1);

  storage.destruct();
  REQUIRE(tracked_value::destroyed >= 1);
}

TEST_CASE("manual_lifetime construct_from forwards factory arguments",
          "[UT][wh/core/stdexec/manual_lifetime.hpp][manual_lifetime::construct_from][branch]") {
  tracked_value::constructed = 0;
  tracked_value::destroyed = 0;

  wh::core::detail::manual_lifetime<tracked_value> storage{};
  auto &value = storage.construct_from(
      [](const int next) noexcept { return tracked_value{next}; }, 17);
  REQUIRE(value.value == 17);
  REQUIRE(tracked_value::constructed >= 1);
  REQUIRE(tracked_value::destroyed == 0);

  storage.destruct();
  REQUIRE(tracked_value::destroyed == 1);
}

TEST_CASE("manual_storage construct_with preserves exact value type",
          "[UT][wh/core/stdexec/manual_lifetime.hpp][manual_storage::construct_with][branch]") {
  tracked_value::constructed = 0;
  tracked_value::destroyed = 0;

  wh::core::detail::manual_storage<sizeof(tracked_value), alignof(tracked_value)>
      storage{};
  auto &value =
      storage.construct_with<tracked_value>([]() noexcept { return tracked_value{31}; });
  REQUIRE(value.value == 31);
  REQUIRE(tracked_value::constructed >= 1);

  storage.destruct<tracked_value>();
  REQUIRE(tracked_value::destroyed >= 1);
}

TEST_CASE("manual_lifetime get on rvalue moves out the stored value",
          "[UT][wh/core/stdexec/manual_lifetime.hpp][manual_lifetime::get][boundary]") {
  tracked_value::constructed = 0;
  tracked_value::destroyed = 0;

  wh::core::detail::manual_lifetime<tracked_value> storage{};
  auto &constructed = storage.construct(23);
  REQUIRE(constructed.value == 23);
  auto moved = std::move(storage).get();
  REQUIRE(moved.value == 23);
  REQUIRE(tracked_value::constructed >= 1);

  storage.destruct();
  REQUIRE(tracked_value::destroyed >= 1);
}
