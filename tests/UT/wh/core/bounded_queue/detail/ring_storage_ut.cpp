#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <utility>

#include "wh/core/bounded_queue/detail/ring_storage.hpp"

namespace {

struct ring_probe {
  static inline int live_count = 0;
  static inline int destroy_count = 0;

  int value{0};

  ring_probe() { ++live_count; }
  explicit ring_probe(int input) : value(input) { ++live_count; }
  ring_probe(const ring_probe &other) : value(other.value) { ++live_count; }
  ring_probe(ring_probe &&other) noexcept : value(other.value) { ++live_count; }
  auto operator=(const ring_probe &) -> ring_probe & = default;
  auto operator=(ring_probe &&) noexcept -> ring_probe & = default;
  ~ring_probe() {
    --live_count;
    ++destroy_count;
  }
};

} // namespace

TEST_CASE("ring storage pushes pops wraps around and supports move transfer",
          "[UT][wh/core/bounded_queue/detail/ring_storage.hpp][ring_storage::push_back][condition][branch][boundary]") {
  ring_probe::live_count = 0;
  ring_probe::destroy_count = 0;

  wh::core::detail::ring_storage<ring_probe> storage{3U};
  REQUIRE(storage.capacity() == 3U);
  REQUIRE(storage.empty());
  REQUIRE_FALSE(storage.full());

  storage.emplace_back(1);
  storage.push_back(ring_probe{2});
  REQUIRE(storage.size() == 2U);

  auto first = storage.pop_front();
  REQUIRE(first.value == 1);
  REQUIRE(storage.size() == 1U);

  storage.emplace_back(3);
  storage.emplace_back(4);
  REQUIRE(storage.full());

  ring_probe consumed{};
  storage.consume_front([&](ring_probe &&value) { consumed = std::move(value); });
  REQUIRE(consumed.value == 2);
  REQUIRE(storage.size() == 2U);

  auto moved = std::move(storage);
  REQUIRE(moved.capacity() == 3U);
  REQUIRE(moved.size() == 2U);
  REQUIRE(storage.capacity() == 0U);
  REQUIRE(storage.size() == 0U);

  REQUIRE(moved.pop_front().value == 3);
  REQUIRE(moved.pop_front().value == 4);
  REQUIRE(moved.empty());
}

TEST_CASE("ring storage destroys consumed slot even when sink throws",
          "[UT][wh/core/bounded_queue/detail/ring_storage.hpp][ring_storage::consume_front][error][branch]") {
  ring_probe::live_count = 0;
  ring_probe::destroy_count = 0;

  wh::core::detail::ring_storage<ring_probe> storage{1U};
  storage.emplace_back(9);

  REQUIRE_THROWS_AS(
      storage.consume_front([](ring_probe &&) { throw std::runtime_error{"boom"}; }),
      std::runtime_error);
  REQUIRE(storage.empty());
  REQUIRE(ring_probe::live_count == 0);
}
