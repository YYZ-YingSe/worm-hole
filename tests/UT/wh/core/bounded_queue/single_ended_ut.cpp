#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <tuple>
#include <type_traits>

#include <stdexec/execution.hpp>

#include "wh/core/bounded_queue/single_ended.hpp"

TEST_CASE("single ended queue factories expose producer and consumer views over the same queue",
          "[UT][wh/core/bounded_queue/single_ended.hpp][split_endpoints][condition][branch]") {
  wh::core::bounded_queue<int> queue{2U};
  auto producer = wh::core::make_producer(queue);
  auto consumer = wh::core::make_consumer(queue);

  STATIC_REQUIRE(std::same_as<decltype(producer)::queue_type,
                              wh::core::bounded_queue<int>>);
  STATIC_REQUIRE(std::same_as<decltype(consumer)::queue_type,
                              wh::core::bounded_queue<int>>);

  REQUIRE(producer.capacity() == 2U);
  REQUIRE(consumer.capacity() == 2U);
  REQUIRE_FALSE(producer.is_closed());
  REQUIRE_FALSE(consumer.is_closed());
  REQUIRE(producer.get_allocator() == queue.get_allocator());
  REQUIRE(consumer.get_allocator() == queue.get_allocator());
}

TEST_CASE("single ended producer and consumer forward sync push pop and close branches",
          "[UT][wh/core/bounded_queue/single_ended.hpp][bounded_queue_producer::try_push][condition][branch][boundary]") {
  wh::core::bounded_queue<int> queue{2U};
  auto [producer, consumer] = wh::core::split_endpoints(queue);
  REQUIRE(producer.push(1));
  REQUIRE(producer.try_emplace(2) == wh::core::bounded_queue_status::success);
  REQUIRE(producer.try_push(3) == wh::core::bounded_queue_status::full);
  REQUIRE(consumer.size_hint() == 2U);
  REQUIRE(consumer.pop() == std::optional<int>{1});
  REQUIRE(consumer.try_pop().value() == 2);
  REQUIRE(consumer.try_pop().error() == wh::core::bounded_queue_status::empty);

  producer.close();
  REQUIRE(producer.is_closed());
  REQUIRE(consumer.is_closed());
  REQUIRE(producer.try_push(7) == wh::core::bounded_queue_status::closed);
  REQUIRE(consumer.try_pop().error() == wh::core::bounded_queue_status::closed);
}

TEST_CASE("single ended endpoints forward async producer consumer operations",
          "[UT][wh/core/bounded_queue/single_ended.hpp][bounded_queue_producer::async_push][branch]") {
  wh::core::bounded_queue<int> queue{2U};
  auto [producer, consumer] = wh::core::split_endpoints(queue);
  auto waited_push = stdexec::sync_wait(producer.async_push(7));
  REQUIRE(waited_push.has_value());
  auto waited_pop = stdexec::sync_wait(consumer.async_pop());
  REQUIRE(waited_pop.has_value());
  REQUIRE(std::get<0>(*waited_pop) == 7);
}
