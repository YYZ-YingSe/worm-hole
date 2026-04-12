#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <stop_token>
#include <tuple>

#include <stdexec/execution.hpp>

#include "helper/manual_scheduler.hpp"
#include "helper/sender_capture.hpp"
#include "helper/sender_env.hpp"
#include "wh/core/bounded_queue/bounded_queue.hpp"

TEST_CASE("bounded queue sync api covers full empty closed and zero-capacity boundaries",
          "[UT][wh/core/bounded_queue/bounded_queue.hpp][bounded_queue::try_push][condition][branch][boundary]") {
  wh::core::bounded_queue<int> queue{2U};

  REQUIRE(queue.capacity() == 2U);
  REQUIRE(queue.size_hint() == 0U);
  REQUIRE_FALSE(queue.is_closed());

  const auto empty = queue.try_pop();
  REQUIRE(empty.has_error());
  REQUIRE(empty.error() == wh::core::bounded_queue_status::empty);

  REQUIRE(queue.try_push(1) == wh::core::bounded_queue_status::success);
  REQUIRE(queue.try_emplace(2) == wh::core::bounded_queue_status::success);
  REQUIRE(queue.size_hint() == 2U);
  REQUIRE(queue.try_push(3) == wh::core::bounded_queue_status::full);

  REQUIRE(queue.pop() == std::optional<int>{1});
  auto second = queue.try_pop();
  REQUIRE(second.has_value());
  REQUIRE(second.value() == 2);
  REQUIRE(queue.try_pop().error() == wh::core::bounded_queue_status::empty);

  queue.close();
  REQUIRE(queue.is_closed());
  REQUIRE(queue.try_push(9) == wh::core::bounded_queue_status::closed);
  REQUIRE_FALSE(queue.pop().has_value());
  REQUIRE(queue.try_pop().error() == wh::core::bounded_queue_status::closed);

  wh::core::bounded_queue<int> rendezvous{0U};
  REQUIRE(rendezvous.capacity() == 0U);
  REQUIRE(rendezvous.try_push(5) == wh::core::bounded_queue_status::full);
  REQUIRE(rendezvous.try_pop().error() == wh::core::bounded_queue_status::empty);
}

TEST_CASE("bounded queue async api wakes waiting push and pop via scheduler env",
          "[UT][wh/core/bounded_queue/bounded_queue.hpp][bounded_queue::async_pop][branch][concurrency]") {
  using scheduler_t =
      wh::testing::helper::manual_scheduler<wh::core::detail::would_block>;
  using env_t = wh::testing::helper::scheduler_env<scheduler_t, std::stop_token>;

  wh::testing::helper::manual_scheduler_state scheduler_state{};
  scheduler_t scheduler{&scheduler_state};
  env_t env{scheduler, {}};

  wh::core::bounded_queue<int> queue{1U};
  REQUIRE(queue.push(1));

  wh::testing::helper::sender_capture<void> push_capture{};
  auto push_operation = stdexec::connect(
      queue.async_push(2),
      wh::testing::helper::sender_capture_receiver<void, env_t>{&push_capture,
                                                                env});
  stdexec::start(push_operation);
  REQUIRE_FALSE(push_capture.ready.try_acquire());

  auto first = queue.try_pop();
  REQUIRE(first.has_value());
  REQUIRE(first.value() == 1);
  REQUIRE(scheduler_state.run_one());
  REQUIRE(push_capture.ready.try_acquire());
  REQUIRE(push_capture.terminal ==
          wh::testing::helper::sender_terminal_kind::value);

  wh::testing::helper::sender_capture<int> pop_capture{};
  auto pop_operation = stdexec::connect(
      queue.async_pop(),
      wh::testing::helper::sender_capture_receiver<int, env_t>{&pop_capture, env});
  stdexec::start(pop_operation);
  if (!pop_capture.ready.try_acquire()) {
    REQUIRE(scheduler_state.run_one());
    REQUIRE(pop_capture.ready.try_acquire());
  }
  REQUIRE(pop_capture.terminal == wh::testing::helper::sender_terminal_kind::value);
  REQUIRE(pop_capture.value.has_value());
  REQUIRE(*pop_capture.value == 2);

  wh::testing::helper::sender_capture<int> closed_capture{};
  auto closed_operation = stdexec::connect(
      queue.async_pop(),
      wh::testing::helper::sender_capture_receiver<int, env_t>{&closed_capture, env});
  stdexec::start(closed_operation);
  REQUIRE_FALSE(closed_capture.ready.try_acquire());
  queue.close();
  REQUIRE(scheduler_state.run_one());
  REQUIRE(closed_capture.ready.try_acquire());
  REQUIRE(closed_capture.terminal ==
          wh::testing::helper::sender_terminal_kind::error);
}
