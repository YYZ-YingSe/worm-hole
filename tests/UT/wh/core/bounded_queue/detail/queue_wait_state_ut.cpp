#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "wh/core/bounded_queue/detail/queue_wait_state.hpp"

namespace {

struct queue_waiter {
  queue_waiter *next{nullptr};
  queue_waiter *prev{nullptr};
  int id{0};
};

} // namespace

TEST_CASE("queue wait state tracks push and pop queues including independent removal branches",
          "[UT][wh/core/bounded_queue/detail/"
          "queue_wait_state.hpp][wait_state::remove_push][condition][branch]") {
  wh::core::detail::wait_state<queue_waiter> state{};
  queue_waiter push_a{.id = 1};
  queue_waiter push_b{.id = 2};
  queue_waiter pop_a{.id = 3};
  queue_waiter pop_b{.id = 4};

  REQUIRE_FALSE(state.is_closed());
  state.enqueue_push(&push_a);
  state.enqueue_push(&push_b);
  state.enqueue_pop(&pop_a);
  state.enqueue_pop(&pop_b);

  REQUIRE(state.front_push() == &push_a);
  REQUIRE(state.front_pop() == &pop_a);
  REQUIRE(state.remove_push(&push_b));
  REQUIRE_FALSE(state.remove_push(&push_b));
  REQUIRE(state.remove_pop(&pop_b));
  REQUIRE_FALSE(state.remove_pop(&pop_b));
  REQUIRE(state.take_push() == &push_a);
  REQUIRE(state.front_push() == nullptr);
  REQUIRE(state.take_pop() == &pop_a);
  REQUIRE(state.front_pop() == nullptr);
}

TEST_CASE("queue wait state close and detach transitions are single-shot and preserve order",
          "[UT][wh/core/bounded_queue/detail/"
          "queue_wait_state.hpp][wait_state::close_and_detach][branch][boundary]") {
  wh::core::detail::wait_state<queue_waiter> state{};
  queue_waiter push_a{.id = 1};
  queue_waiter push_b{.id = 2};
  queue_waiter pop_a{.id = 3};
  state.enqueue_push(&push_a);
  state.enqueue_push(&push_b);
  state.enqueue_pop(&pop_a);

  auto detached = state.close_and_detach();
  REQUIRE(detached.has_value());
  REQUIRE(state.is_closed());
  REQUIRE(detached->push_head == &push_a);
  REQUIRE(detached->pop_head == &pop_a);

  std::vector<int> detached_push{};
  for (auto *iter = detached->push_head; iter != nullptr; iter = iter->next) {
    detached_push.push_back(iter->id);
  }
  REQUIRE(detached_push == std::vector<int>{1, 2});

  REQUIRE_FALSE(state.close_and_detach().has_value());
  REQUIRE_FALSE(state.close());
}

TEST_CASE("queue wait state close on empty state is idempotent",
          "[UT][wh/core/bounded_queue/detail/queue_wait_state.hpp][wait_state::close][branch]") {
  wh::core::detail::wait_state<queue_waiter> state{};
  REQUIRE(state.close());
  REQUIRE(state.is_closed());
  REQUIRE_FALSE(state.close());
  REQUIRE_FALSE(state.close_and_detach().has_value());
}
