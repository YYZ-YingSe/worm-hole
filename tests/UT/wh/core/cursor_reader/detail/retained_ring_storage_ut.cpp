#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "wh/core/cursor_reader/detail/retained_ring_storage.hpp"

namespace {

struct retained_probe {
  static inline int live_count = 0;

  int value{0};

  retained_probe() { ++live_count; }
  explicit retained_probe(int input) : value(input) { ++live_count; }
  retained_probe(const retained_probe &other) : value(other.value) { ++live_count; }
  retained_probe(retained_probe &&other) noexcept : value(other.value) {
    ++live_count;
  }
  ~retained_probe() { --live_count; }
};

} // namespace

TEST_CASE("retained ring storage constructs indexes reserves and moves retained slots",
          "[UT][wh/core/cursor_reader/detail/retained_ring_storage.hpp][retained_ring_storage::reserve][condition][branch][boundary]") {
  retained_probe::live_count = 0;

  wh::core::cursor_reader_detail::retained_ring_storage<retained_probe> storage{2U};
  REQUIRE(storage.capacity() == 2U);

  storage.construct_at_sequence(0U, 7);
  storage.construct_at_sequence(1U, 9);
  REQUIRE(storage.value_at_sequence(0U).value == 7);
  REQUIRE(storage.value_at_sequence(1U).value == 9);

  storage.reserve(4U, 0U, 2U);
  REQUIRE(storage.capacity() == 4U);
  REQUIRE(storage.value_at_sequence(0U).value == 7);
  REQUIRE(storage.value_at_sequence(1U).value == 9);

  auto moved = std::move(storage);
  REQUIRE(moved.capacity() == 4U);
  REQUIRE(storage.capacity() == 0U);
  REQUIRE(moved.value_at_sequence(0U).value == 7);
  REQUIRE(moved.value_at_sequence(1U).value == 9);

  moved.destroy_at_sequence(0U);
  moved.destroy_at_sequence(1U);
  REQUIRE(retained_probe::live_count == 0);
}

TEST_CASE("retained ring storage ignores no-op reserve requests and supports move assignment",
          "[UT][wh/core/cursor_reader/detail/retained_ring_storage.hpp][retained_ring_storage::operator=][condition][branch][boundary]") {
  retained_probe::live_count = 0;

  wh::core::cursor_reader_detail::retained_ring_storage<retained_probe> storage{1U};
  storage.construct_at_sequence(2U, 5);
  storage.reserve(1U, 2U, 3U);
  REQUIRE(storage.capacity() == 1U);
  REQUIRE(storage.value_at_sequence(2U).value == 5);

  wh::core::cursor_reader_detail::retained_ring_storage<retained_probe> moved{0U};
  moved = std::move(storage);
  REQUIRE(moved.capacity() == 1U);
  REQUIRE(storage.capacity() == 0U);
  REQUIRE(moved.value_at_sequence(2U).value == 5);

  moved.destroy_at_sequence(2U);
  REQUIRE(retained_probe::live_count == 0);
}
