#include <memory>

#include <catch2/catch_test_macros.hpp>

#include "wh/core/cursor_reader/detail/retained_ring_storage.hpp"

namespace {

struct retained_probe {
  static inline int live_count = 0;

  int value{0};

  retained_probe() { ++live_count; }
  explicit retained_probe(int input) : value(input) { ++live_count; }
  retained_probe(const retained_probe &other) : value(other.value) { ++live_count; }
  retained_probe(retained_probe &&other) noexcept : value(other.value) { ++live_count; }
  ~retained_probe() { --live_count; }
};

} // namespace

TEST_CASE(
    "retained ring storage constructs indexes reserves and moves retained slots",
    "[UT][wh/core/cursor_reader/detail/"
    "retained_ring_storage.hpp][retained_ring_storage::reserve][condition][branch][boundary]") {
  retained_probe::live_count = 0;

  wh::core::cursor_reader_detail::retained_ring_storage<retained_probe> storage{2U};
  REQUIRE(storage.capacity() == 2U);
  REQUIRE(storage.empty());
  REQUIRE(storage.front_sequence() == 0U);
  REQUIRE(storage.end_sequence() == 0U);

  storage.emplace_back(7);
  storage.emplace_back(9);
  REQUIRE(storage.size() == 2U);
  REQUIRE(storage.front_sequence() == 0U);
  REQUIRE(storage.end_sequence() == 2U);
  REQUIRE(storage.value_at_sequence(0U).value == 7);
  REQUIRE(storage.value_at_sequence(1U).value == 9);

  storage.reserve(4U);
  REQUIRE(storage.capacity() == 4U);
  REQUIRE(storage.size() == 2U);
  REQUIRE(storage.front_sequence() == 0U);
  REQUIRE(storage.end_sequence() == 2U);
  REQUIRE(storage.value_at_sequence(0U).value == 7);
  REQUIRE(storage.value_at_sequence(1U).value == 9);

  auto moved = std::move(storage);
  REQUIRE(moved.capacity() == 4U);
  REQUIRE(moved.size() == 2U);
  REQUIRE(moved.front_sequence() == 0U);
  REQUIRE(moved.end_sequence() == 2U);
  REQUIRE(storage.capacity() == 0U);
  REQUIRE(moved.value_at_sequence(0U).value == 7);
  REQUIRE(moved.value_at_sequence(1U).value == 9);

  moved.destroy_front();
  REQUIRE(moved.front_sequence() == 1U);
  REQUIRE(moved.end_sequence() == 2U);
  moved.destroy_front();
  REQUIRE(moved.empty());
  REQUIRE(moved.front_sequence() == 2U);
  REQUIRE(moved.end_sequence() == 2U);
  REQUIRE(retained_probe::live_count == 0);
}

TEST_CASE(
    "retained ring storage ignores no-op reserve requests and supports move assignment",
    "[UT][wh/core/cursor_reader/detail/"
    "retained_ring_storage.hpp][retained_ring_storage::operator=][condition][branch][boundary]") {
  retained_probe::live_count = 0;

  wh::core::cursor_reader_detail::retained_ring_storage<retained_probe> storage{1U};
  storage.emplace_back(5);
  storage.reserve(1U);
  REQUIRE(storage.capacity() == 1U);
  REQUIRE(storage.value_at_sequence(0U).value == 5);

  wh::core::cursor_reader_detail::retained_ring_storage<retained_probe> moved{0U};
  moved = std::move(storage);
  REQUIRE(moved.capacity() == 1U);
  REQUIRE(storage.capacity() == 0U);
  REQUIRE(moved.value_at_sequence(0U).value == 5);

  moved.destroy_front();
  REQUIRE(retained_probe::live_count == 0);
}

TEST_CASE("retained ring storage reserve preserves live sequences after prefix reclaim",
          "[UT][wh/core/cursor_reader/detail/"
          "retained_ring_storage.hpp][retained_ring_storage::reserve][lifetime][regression]") {
  retained_probe::live_count = 0;

  wh::core::cursor_reader_detail::retained_ring_storage<retained_probe> storage{4U};
  storage.emplace_back(10);
  storage.emplace_back(11);
  storage.emplace_back(12);
  storage.emplace_back(13);

  storage.destroy_front();
  storage.destroy_front();
  REQUIRE(storage.front_sequence() == 2U);
  REQUIRE(storage.end_sequence() == 4U);

  storage.emplace_back(14);
  storage.emplace_back(15);
  REQUIRE(storage.front_sequence() == 2U);
  REQUIRE(storage.end_sequence() == 6U);

  storage.reserve(8U);
  REQUIRE(storage.capacity() == 8U);
  REQUIRE(storage.size() == 4U);
  REQUIRE(storage.front_sequence() == 2U);
  REQUIRE(storage.end_sequence() == 6U);
  REQUIRE(storage.value_at_sequence(2U).value == 12);
  REQUIRE(storage.value_at_sequence(3U).value == 13);
  REQUIRE(storage.value_at_sequence(4U).value == 14);
  REQUIRE(storage.value_at_sequence(5U).value == 15);

  storage.destroy_front();
  storage.destroy_front();
  storage.destroy_front();
  storage.destroy_front();
  REQUIRE(storage.empty());
  REQUIRE(retained_probe::live_count == 0);
}

TEST_CASE("retained ring storage destroys remaining live slots on scope exit",
          "[UT][wh/core/cursor_reader/detail/"
          "retained_ring_storage.hpp][retained_ring_storage::~retained_ring_storage][lifetime]["
          "regression]") {
  retained_probe::live_count = 0;

  {
    wh::core::cursor_reader_detail::retained_ring_storage<retained_probe> storage{4U};
    storage.emplace_back(17);
    storage.emplace_back(23);

    REQUIRE(retained_probe::live_count == 2);
    REQUIRE(storage.value_at_sequence(0U).value == 17);
    REQUIRE(storage.value_at_sequence(1U).value == 23);
  }

  REQUIRE(retained_probe::live_count == 0);
}
