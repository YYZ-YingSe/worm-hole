#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "wh/core/stdexec/manual_lifetime.hpp"

namespace {

struct tracked_value {
  static inline int constructed = 0;
  static inline int destroyed = 0;

  int value{0};

  explicit tracked_value(const int next) noexcept : value(next) { ++constructed; }

  tracked_value(const tracked_value &) = delete;
  auto operator=(const tracked_value &) -> tracked_value & = delete;

  tracked_value(tracked_value &&other) noexcept : value(other.value) { ++constructed; }

  ~tracked_value() noexcept { ++destroyed; }
};

struct immovable_pointer_value {
  int *pointer{nullptr};
  int marker{0};

  explicit immovable_pointer_value(int *next_pointer, const int next_marker = 0) noexcept
      : pointer(next_pointer), marker(next_marker) {}

  immovable_pointer_value(const immovable_pointer_value &) = delete;
  auto operator=(const immovable_pointer_value &) -> immovable_pointer_value & = delete;
  immovable_pointer_value(immovable_pointer_value &&) = delete;
  auto operator=(immovable_pointer_value &&) -> immovable_pointer_value & = delete;
};

} // namespace

TEST_CASE("manual_lifetime constructs and destructs values explicitly",
          "[UT][wh/core/stdexec/"
          "manual_lifetime.hpp][manual_lifetime::construct][manual_lifetime::destruct]") {
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
          "[UT][wh/core/stdexec/"
          "manual_lifetime.hpp][manual_storage::construct][manual_storage::destruct]") {
  tracked_value::constructed = 0;
  tracked_value::destroyed = 0;

  wh::core::detail::manual_storage<sizeof(tracked_value), alignof(tracked_value)> storage{};
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
  auto &value =
      storage.construct_from([](const int next) noexcept { return tracked_value{next}; }, 17);
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

  wh::core::detail::manual_storage<sizeof(tracked_value), alignof(tracked_value)> storage{};
  auto &value = storage.construct_with<tracked_value>([]() noexcept { return tracked_value{31}; });
  REQUIRE(value.value == 31);
  REQUIRE(tracked_value::constructed >= 1);

  storage.destruct<tracked_value>();
  REQUIRE(tracked_value::destroyed >= 1);
}

TEST_CASE("manual_storage construct_with preserves pointer fields for "
          "immovable values",
          "[UT][wh/core/stdexec/manual_lifetime.hpp][manual_storage::construct_with][boundary]") {
  int payload = 41;

  wh::core::detail::manual_storage<sizeof(immovable_pointer_value),
                                   alignof(immovable_pointer_value)>
      storage{};
  auto &value = storage.construct_with<immovable_pointer_value>(
      [&]() noexcept { return immovable_pointer_value{&payload, 77}; });

  REQUIRE(value.pointer == &payload);
  REQUIRE(*value.pointer == 41);
  REQUIRE(value.marker == 77);

  storage.destruct<immovable_pointer_value>();
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
