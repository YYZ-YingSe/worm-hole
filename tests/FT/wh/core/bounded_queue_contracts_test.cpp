#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <semaphore>
#include <stop_token>
#include <thread>

#include <stdexec/execution.hpp>

#include "helper/manual_scheduler.hpp"
#include "helper/sender_capture.hpp"
#include "helper/sender_env.hpp"
#include "wh/core/bounded_queue.hpp"

namespace {

struct would_block {};

using scheduler_t = wh::testing::helper::manual_scheduler<would_block>;
using scheduler_env_t =
    wh::testing::helper::scheduler_env<scheduler_t, std::stop_token>;

} // namespace

TEST_CASE("bounded queue public facade preserves rendezvous handoff and close across threads",
          "[core][bounded_queue][functional][concurrency]") {
  using namespace std::chrono_literals;

  wh::core::bounded_queue<int> queue{0U};
  std::binary_semaphore consumer_ready{0};
  std::binary_semaphore consumer_done{0};
  std::optional<int> consumed{};

  std::jthread consumer([&] {
    consumer_ready.release();
    auto first = queue.pop();
    REQUIRE(first.has_value());
    consumed = *first;

    auto eof = queue.pop();
    REQUIRE_FALSE(eof.has_value());
    consumer_done.release();
  });

  REQUIRE(consumer_ready.try_acquire_for(1s));
  REQUIRE(queue.push(7));
  queue.close();
  REQUIRE(consumer_done.try_acquire_for(1s));
  REQUIRE(consumed == std::optional<int>{7});
}

TEST_CASE("bounded queue async public facade wakes pending push and pop on scheduler env",
          "[core][bounded_queue][functional][concurrency][boundary]") {
  wh::testing::helper::manual_scheduler_state scheduler_state{};
  scheduler_t scheduler{&scheduler_state};
  scheduler_env_t env{scheduler, {}};

  wh::core::bounded_queue<int> queue{1U};
  REQUIRE(queue.push(1));

  wh::testing::helper::sender_capture<void> push_capture{};
  auto push_operation = stdexec::connect(
      queue.async_push(2),
      wh::testing::helper::sender_capture_receiver<void, scheduler_env_t>{
          &push_capture, env});
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
      wh::testing::helper::sender_capture_receiver<int, scheduler_env_t>{
          &pop_capture, env});
  stdexec::start(pop_operation);
  if (!pop_capture.ready.try_acquire()) {
    REQUIRE(scheduler_state.run_one());
    REQUIRE(pop_capture.ready.try_acquire());
  }
  REQUIRE(pop_capture.terminal ==
          wh::testing::helper::sender_terminal_kind::value);
  REQUIRE(pop_capture.value.has_value());
  REQUIRE(*pop_capture.value == 2);

  wh::testing::helper::sender_capture<int> closed_capture{};
  auto closed_operation = stdexec::connect(
      queue.async_pop(),
      wh::testing::helper::sender_capture_receiver<int, scheduler_env_t>{
          &closed_capture, env});
  stdexec::start(closed_operation);
  REQUIRE_FALSE(closed_capture.ready.try_acquire());
  queue.close();
  REQUIRE(scheduler_state.run_one());
  REQUIRE(closed_capture.ready.try_acquire());
  REQUIRE(closed_capture.terminal ==
          wh::testing::helper::sender_terminal_kind::error);
}
