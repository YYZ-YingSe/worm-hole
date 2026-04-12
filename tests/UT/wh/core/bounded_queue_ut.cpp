#include <catch2/catch_test_macros.hpp>

#include <type_traits>

#include "wh/core/bounded_queue.hpp"

static_assert(std::is_class_v<wh::core::bounded_queue<int>>);
static_assert(std::is_class_v<wh::core::bounded_queue_producer<int>>);
static_assert(std::is_class_v<wh::core::bounded_queue_consumer<int>>);

TEST_CASE("bounded_queue facade exports queue types and status enum text",
          "[UT][wh/core/bounded_queue.hpp][bounded_queue_status][branch][boundary]") {
  REQUIRE(wh::core::to_string(wh::core::bounded_queue_status::success) ==
          "success");
}

TEST_CASE("bounded_queue facade reexports producer consumer endpoint helpers",
          "[UT][wh/core/bounded_queue.hpp][split_endpoints][condition][branch][boundary]") {
  wh::core::bounded_queue<int> queue{2U};
  auto [producer, consumer] = wh::core::split_endpoints(queue);

  REQUIRE(producer.try_push(3) == wh::core::bounded_queue_status::success);
  auto popped = consumer.try_pop();
  REQUIRE(popped.has_value());
  REQUIRE(*popped == 3);
}
