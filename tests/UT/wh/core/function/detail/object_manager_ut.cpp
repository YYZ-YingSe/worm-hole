#include <catch2/catch_test_macros.hpp>

#include <atomic>

#include "wh/core/function/detail/object_manager.hpp"

namespace {

struct tracked_value {
  inline static std::atomic<int> copies{0};
  inline static std::atomic<int> destructions{0};

  static auto reset() -> void {
    copies.store(0);
    destructions.store(0);
  }

  tracked_value() = default;
  explicit tracked_value(int next) : value(next) {}
  tracked_value(const tracked_value &other) : value(other.value) {
    copies.fetch_add(1);
  }
  auto operator=(const tracked_value &) -> tracked_value & = default;
  ~tracked_value() { destructions.fetch_add(1); }

  int value{0};
};

using deep_manager = wh::core::fn_detail::object_manager<
    tracked_value, wh::core::fn::deep_copy, wh::core::fn::skip_on_error,
    sizeof(void *)>;
using local_manager = wh::core::fn_detail::object_manager<
    int, wh::core::fn::reference_counting, wh::core::fn::skip_on_error,
    sizeof(void *)>;

} // namespace

TEST_CASE("object_manager copies moves and tracks invalid moved-from state",
          "[UT][wh/core/function/detail/object_manager.hpp][object_manager][branch]") {
  tracked_value::reset();

  deep_manager first{5};
  deep_manager copied{first};
  REQUIRE(first.access().value == 5);
  REQUIRE(copied.access().value == 5);
  REQUIRE(tracked_value::copies.load() >= 1);

  deep_manager moved{std::move(first)};
  REQUIRE(moved.access().value == 5);
  REQUIRE(first.is_invalid());
}

TEST_CASE("object_manager invalidate marks local storage unusable without crashing destroy path",
          "[UT][wh/core/function/detail/object_manager.hpp][object_manager::invalidate][boundary]") {
  local_manager value{7};
  REQUIRE_FALSE(value.is_invalid());
  REQUIRE(value.access() == 7);

  value.invalidate();
  REQUIRE(value.is_invalid());
}

TEST_CASE("object_manager releases tracked storage on scope exit",
          "[UT][wh/core/function/detail/object_manager.hpp][object_manager::access][condition][boundary]") {
  tracked_value::reset();

  {
    deep_manager value{tracked_value{9}};
    REQUIRE(value.access().value == 9);
    REQUIRE_FALSE(value.is_invalid());
  }

  REQUIRE(tracked_value::destructions.load() >= 1);
}
