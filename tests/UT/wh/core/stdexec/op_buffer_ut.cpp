#include <catch2/catch_test_macros.hpp>

#include "wh/core/stdexec/op_buffer.hpp"

namespace {

struct buffered_probe {
  static inline int live_count = 0;
  static inline int destroy_count = 0;

  int value{0};

  explicit buffered_probe(int input) : value(input) { ++live_count; }
  buffered_probe(const buffered_probe &other) : value(other.value) { ++live_count; }
  buffered_probe(buffered_probe &&other) noexcept : value(other.value) { ++live_count; }
  ~buffered_probe() {
    --live_count;
    ++destroy_count;
  }
};

} // namespace

TEST_CASE("op buffer ensure indexing growth and reset manage boxed operations",
          "[UT][wh/core/stdexec/op_buffer.hpp][op_buffer::ensure][condition][branch][boundary]") {
  buffered_probe::live_count = 0;
  buffered_probe::destroy_count = 0;

  wh::core::detail::op_buffer<buffered_probe> buffer{};
  buffer.ensure(2U);
  buffer[1U].emplace(11);
  buffer[0U].emplace(7);

  REQUIRE(buffer[0U].has_value());
  REQUIRE(buffer[1U].has_value());
  REQUIRE(buffer[0U].get().value == 7);
  REQUIRE(buffer[1U].get().value == 11);
  REQUIRE(buffered_probe::live_count == 2);

  buffer.ensure(1U);
  REQUIRE(buffer[0U].get().value == 7);
  REQUIRE(buffered_probe::live_count == 2);

  buffer.ensure(4U);
  REQUIRE(buffered_probe::live_count == 0);
  REQUIRE_FALSE(buffer[0U].has_value());

  buffer[3U].emplace(99);
  REQUIRE(buffer[3U].has_value());
  REQUIRE(buffer[3U].get().value == 99);
  REQUIRE(buffered_probe::live_count == 1);

  buffer.reset();
  REQUIRE(buffered_probe::live_count == 0);
}

TEST_CASE("op buffer reset is idempotent and operator access grows active window lazily",
          "[UT][wh/core/stdexec/op_buffer.hpp][op_buffer::reset][condition][branch][boundary]") {
  buffered_probe::live_count = 0;
  buffered_probe::destroy_count = 0;

  wh::core::detail::op_buffer<buffered_probe> buffer{};
  buffer.ensure(3U);
  REQUIRE_FALSE(buffer[2U].has_value());

  buffer[2U].emplace(21);
  REQUIRE(buffer[2U].has_value());
  REQUIRE(buffer[2U].get().value == 21);
  REQUIRE(buffered_probe::live_count == 1);

  buffer.reset();
  REQUIRE(buffered_probe::live_count == 0);
  buffer.reset();
  REQUIRE(buffered_probe::live_count == 0);
}
