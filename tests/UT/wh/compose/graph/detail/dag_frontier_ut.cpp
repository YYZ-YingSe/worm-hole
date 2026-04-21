#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph/detail/dag_frontier.hpp"

TEST_CASE("dag frontier enqueues current and next waves without duplicate scheduling",
          "[UT][wh/compose/graph/detail/"
          "dag_frontier.hpp][dag_frontier::promote_next_wave][condition][branch][boundary]") {
  wh::compose::detail::dag_frontier frontier{};
  frontier.reset(6U);

  REQUIRE(frontier.enqueue_current(1U));
  REQUIRE_FALSE(frontier.enqueue_current(1U));
  REQUIRE(frontier.enqueue_current(3U));
  REQUIRE(frontier.dequeue() == std::optional<std::uint32_t>{1U});
  REQUIRE(frontier.dequeue() == std::optional<std::uint32_t>{3U});
  REQUIRE(frontier.dequeue() == std::nullopt);

  REQUIRE(frontier.enqueue_next(4U));
  REQUIRE_FALSE(frontier.enqueue_next(4U));
  REQUIRE(frontier.enqueue_next(5U));
  REQUIRE(frontier.promote_next_wave());
  REQUIRE(frontier.dequeue() == std::optional<std::uint32_t>{4U});
  REQUIRE(frontier.dequeue() == std::optional<std::uint32_t>{5U});
  REQUIRE(frontier.dequeue() == std::nullopt);
  REQUIRE_FALSE(frontier.promote_next_wave());

  frontier.reset(2U);
  REQUIRE_FALSE(frontier.dequeue().has_value());
}

TEST_CASE("dag frontier allows a dequeued node to be scheduled again in the same wave",
          "[UT][wh/compose/graph/detail/"
          "dag_frontier.hpp][dag_frontier::enqueue_current][branch][boundary]") {
  wh::compose::detail::dag_frontier frontier{};
  frontier.reset(3U);

  REQUIRE(frontier.enqueue_current(1U));
  REQUIRE(frontier.dequeue() == std::optional<std::uint32_t>{1U});
  REQUIRE(frontier.enqueue_current(1U));
  REQUIRE_FALSE(frontier.enqueue_next(1U));
  REQUIRE(frontier.dequeue() == std::optional<std::uint32_t>{1U});
  REQUIRE(frontier.dequeue() == std::nullopt);
}

TEST_CASE("dag frontier reset clears queued bookkeeping across current and next waves",
          "[UT][wh/compose/graph/detail/"
          "dag_frontier.hpp][dag_frontier::reset][condition][branch][boundary]") {
  wh::compose::detail::dag_frontier frontier{};
  frontier.reset(4U);

  REQUIRE(frontier.enqueue_current(1U));
  REQUIRE(frontier.enqueue_next(2U));

  frontier.reset(4U);
  REQUIRE(frontier.enqueue_current(1U));
  REQUIRE(frontier.enqueue_next(2U));
  REQUIRE(frontier.dequeue() == std::optional<std::uint32_t>{1U});
  REQUIRE(frontier.dequeue() == std::nullopt);
  REQUIRE(frontier.promote_next_wave());
  REQUIRE(frontier.dequeue() == std::optional<std::uint32_t>{2U});
  REQUIRE_FALSE(frontier.promote_next_wave());
}
