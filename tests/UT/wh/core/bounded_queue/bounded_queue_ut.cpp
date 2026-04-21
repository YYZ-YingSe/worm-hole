#include <chrono>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <tuple>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "helper/manual_scheduler.hpp"
#include "helper/sender_capture.hpp"
#include "helper/sender_env.hpp"
#include "wh/core/bounded_queue/bounded_queue.hpp"

namespace {

struct throwing_scheduler_state {
  std::size_t connect_calls{0U};
  std::optional<std::size_t> fail_on_connect{};
};

struct throwing_scheduler {
  using scheduler_concept = stdexec::scheduler_t;

  template <typename receiver_t> struct schedule_op {
    using operation_state_concept = stdexec::operation_state_t;

    receiver_t receiver;

    auto start() noexcept -> void { stdexec::set_value(std::move(receiver)); }
  };

  struct schedule_sender {
    throwing_scheduler_state *state{nullptr};

    using sender_concept = stdexec::sender_t;
    using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t()>;

    template <typename receiver_t>
    auto connect(receiver_t receiver) const -> schedule_op<receiver_t> {
      ++state->connect_calls;
      if (state->fail_on_connect.has_value() && state->connect_calls == *state->fail_on_connect) {
        throw std::runtime_error("bounded_queue handoff connect failed");
      }
      return schedule_op<receiver_t>{std::move(receiver)};
    }

    [[nodiscard]] auto get_env() const noexcept -> stdexec::env<> { return {}; }
  };

  throwing_scheduler_state *state{nullptr};

  [[nodiscard]] auto schedule() const noexcept -> schedule_sender { return schedule_sender{state}; }

  [[nodiscard]] auto operator==(const throwing_scheduler &) const noexcept -> bool = default;
};

struct throwing_stop_token {
  template <typename callback_t> struct callback_type {
    explicit callback_type(throwing_stop_token, callback_t) {
      throw std::runtime_error{"bounded_queue stop callback failed"};
    }
  };

  bool stop_requested() const noexcept { return false; }
  bool stop_possible() const noexcept { return true; }

  auto operator==(const throwing_stop_token &) const noexcept -> bool = default;
};

} // namespace

TEST_CASE("bounded queue sync api covers full empty closed and zero-capacity boundaries",
          "[UT][wh/core/bounded_queue/"
          "bounded_queue.hpp][bounded_queue::try_push][condition][branch][boundary]") {
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
          "[UT][wh/core/bounded_queue/"
          "bounded_queue.hpp][bounded_queue::async_pop][branch][concurrency]") {
  using scheduler_t = wh::testing::helper::manual_scheduler<wh::core::detail::would_block>;
  using env_t = wh::testing::helper::scheduler_env<scheduler_t, wh::testing::helper::stop_token>;

  wh::testing::helper::manual_scheduler_state scheduler_state{};
  scheduler_t scheduler{&scheduler_state};
  env_t env{scheduler, {}};

  wh::core::bounded_queue<int> queue{1U};
  REQUIRE(queue.push(1));

  wh::testing::helper::sender_capture<void> push_capture{};
  auto push_operation = stdexec::connect(
      queue.async_push(2),
      wh::testing::helper::sender_capture_receiver<void, env_t>{&push_capture, env});
  stdexec::start(push_operation);
  REQUIRE_FALSE(push_capture.ready.try_acquire());

  auto first = queue.try_pop();
  REQUIRE(first.has_value());
  REQUIRE(first.value() == 1);
  REQUIRE(scheduler_state.run_one());
  REQUIRE(push_capture.ready.try_acquire());
  REQUIRE(push_capture.terminal == wh::testing::helper::sender_terminal_kind::value);

  wh::testing::helper::sender_capture<int> pop_capture{};
  auto pop_operation =
      stdexec::connect(queue.async_pop(),
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
  REQUIRE(closed_capture.terminal == wh::testing::helper::sender_terminal_kind::error);
}

TEST_CASE(
    "bounded queue async push reports handoff construction failure before waiter publication",
    "[UT][wh/core/bounded_queue/bounded_queue.hpp][bounded_queue::async_push][error][scheduler]") {
  using env_t = wh::testing::helper::scheduler_env<throwing_scheduler>;

  throwing_scheduler_state scheduler_state{
      .connect_calls = 0U,
      .fail_on_connect = 1U,
  };
  env_t env{throwing_scheduler{&scheduler_state}, {}};

  wh::core::bounded_queue<int> queue{1U};
  REQUIRE(queue.push(1));

  wh::testing::helper::sender_capture<void> push_capture{};
  auto push_operation = stdexec::connect(
      queue.async_push(2),
      wh::testing::helper::sender_capture_receiver<void, env_t>{&push_capture, env});
  stdexec::start(push_operation);

  REQUIRE(push_capture.ready.try_acquire());
  REQUIRE(push_capture.terminal == wh::testing::helper::sender_terminal_kind::error);
  REQUIRE(push_capture.error != nullptr);
  REQUIRE(scheduler_state.connect_calls == 2U);
  REQUIRE_FALSE(push_capture.ready.try_acquire());

  auto popped = queue.try_pop();
  REQUIRE(popped.has_value());
  REQUIRE(*popped == 1);
  REQUIRE(queue.try_push(3) == wh::core::bounded_queue_status::success);
}

TEST_CASE(
    "bounded queue async pop reports handoff construction failure before waiter publication",
    "[UT][wh/core/bounded_queue/bounded_queue.hpp][bounded_queue::async_pop][error][scheduler]") {
  using env_t = wh::testing::helper::scheduler_env<throwing_scheduler>;

  throwing_scheduler_state scheduler_state{
      .connect_calls = 0U,
      .fail_on_connect = 1U,
  };
  env_t env{throwing_scheduler{&scheduler_state}, {}};

  wh::core::bounded_queue<int> queue{1U};

  wh::testing::helper::sender_capture<int> pop_capture{};
  auto pop_operation =
      stdexec::connect(queue.async_pop(),
                       wh::testing::helper::sender_capture_receiver<int, env_t>{&pop_capture, env});
  stdexec::start(pop_operation);

  REQUIRE(pop_capture.ready.try_acquire());
  REQUIRE(pop_capture.terminal == wh::testing::helper::sender_terminal_kind::error);
  REQUIRE(pop_capture.error != nullptr);
  REQUIRE(scheduler_state.connect_calls == 2U);
  REQUIRE_FALSE(pop_capture.ready.try_acquire());

  REQUIRE(queue.try_push(7) == wh::core::bounded_queue_status::success);
  auto popped = queue.try_pop();
  REQUIRE(popped.has_value());
  REQUIRE(*popped == 7);
}

TEST_CASE(
    "bounded queue async push reports stop callback construction failure before waiter publication",
    "[UT][wh/core/bounded_queue/bounded_queue.hpp][bounded_queue::async_push][error][stop]") {
  using env_t = wh::testing::helper::scheduler_env<stdexec::inline_scheduler, throwing_stop_token>;

  wh::core::bounded_queue<int> queue{1U};
  REQUIRE(queue.push(1));

  wh::testing::helper::sender_capture<void> push_capture{};
  auto push_operation = stdexec::connect(
      queue.async_push(2),
      wh::testing::helper::sender_capture_receiver<void, env_t>{
          &push_capture, {.scheduler = stdexec::inline_scheduler{}, .stop_token = {}}});
  stdexec::start(push_operation);

  REQUIRE(push_capture.ready.try_acquire());
  REQUIRE(push_capture.terminal == wh::testing::helper::sender_terminal_kind::error);
  REQUIRE(push_capture.error != nullptr);
  REQUIRE_FALSE(push_capture.ready.try_acquire());

  auto queued = queue.try_pop();
  REQUIRE(queued.has_value());
  REQUIRE(*queued == 1);
  REQUIRE(queue.try_push(3) == wh::core::bounded_queue_status::success);
  REQUIRE_FALSE(push_capture.ready.try_acquire());
}

TEST_CASE(
    "bounded queue async pop reports stop callback construction failure before waiter publication",
    "[UT][wh/core/bounded_queue/bounded_queue.hpp][bounded_queue::async_pop][error][stop]") {
  using env_t = wh::testing::helper::scheduler_env<stdexec::inline_scheduler, throwing_stop_token>;

  wh::core::bounded_queue<int> queue{1U};

  wh::testing::helper::sender_capture<int> pop_capture{};
  auto pop_operation = stdexec::connect(
      queue.async_pop(),
      wh::testing::helper::sender_capture_receiver<int, env_t>{
          &pop_capture, {.scheduler = stdexec::inline_scheduler{}, .stop_token = {}}});
  stdexec::start(pop_operation);

  REQUIRE(pop_capture.ready.try_acquire());
  REQUIRE(pop_capture.terminal == wh::testing::helper::sender_terminal_kind::error);
  REQUIRE(pop_capture.error != nullptr);
  REQUIRE_FALSE(pop_capture.ready.try_acquire());

  REQUIRE(queue.try_push(7) == wh::core::bounded_queue_status::success);
  REQUIRE_FALSE(pop_capture.ready.try_acquire());
  auto popped = queue.try_pop();
  REQUIRE(popped.has_value());
  REQUIRE(*popped == 7);
}

TEST_CASE("bounded queue same-scheduler async pop delivers stopped inline without handoff state",
          "[UT][wh/core/bounded_queue/"
          "bounded_queue.hpp][bounded_queue::async_pop][stop][same_scheduler]") {
  using scheduler_t = wh::testing::helper::manual_scheduler<wh::core::detail::would_block>;
  using env_t = wh::testing::helper::scheduler_env<scheduler_t, stdexec::inplace_stop_token>;

  wh::testing::helper::manual_scheduler_state scheduler_state{};
  scheduler_state.same_scheduler = true;
  scheduler_t scheduler{&scheduler_state};
  stdexec::inplace_stop_source stop_source{};
  env_t env{scheduler, stop_source.get_token()};

  wh::core::bounded_queue<int> queue{1U};

  wh::testing::helper::sender_capture<int> pop_capture{};
  auto pop_operation =
      stdexec::connect(queue.async_pop(),
                       wh::testing::helper::sender_capture_receiver<int, env_t>{&pop_capture, env});
  stdexec::start(pop_operation);

  stop_source.request_stop();

  REQUIRE(pop_capture.ready.try_acquire_for(std::chrono::milliseconds(100)));
  REQUIRE(pop_capture.terminal == wh::testing::helper::sender_terminal_kind::stopped);
  REQUIRE(scheduler_state.pending_count() == 0U);
}

TEST_CASE("bounded queue same-scheduler wake completes waiting push inline without scheduling work",
          "[UT][wh/core/bounded_queue/"
          "bounded_queue.hpp][bounded_queue::async_push][wake][same_scheduler]") {
  using scheduler_t = wh::testing::helper::manual_scheduler<wh::core::detail::would_block>;
  using env_t = wh::testing::helper::scheduler_env<scheduler_t>;

  wh::testing::helper::manual_scheduler_state scheduler_state{};
  scheduler_state.same_scheduler = true;
  scheduler_t scheduler{&scheduler_state};
  env_t env{scheduler, {}};

  wh::core::bounded_queue<int> queue{1U};
  REQUIRE(queue.push(1));

  wh::testing::helper::sender_capture<void> push_capture{};
  auto push_operation = stdexec::connect(
      queue.async_push(2),
      wh::testing::helper::sender_capture_receiver<void, env_t>{&push_capture, env});
  stdexec::start(push_operation);
  REQUIRE_FALSE(push_capture.ready.try_acquire());

  auto first = queue.try_pop();
  REQUIRE(first.has_value());
  REQUIRE(*first == 1);

  REQUIRE(push_capture.ready.try_acquire_for(std::chrono::milliseconds(100)));
  REQUIRE(push_capture.terminal == wh::testing::helper::sender_terminal_kind::value);
  REQUIRE(scheduler_state.pending_count() == 0U);

  auto second = queue.try_pop();
  REQUIRE(second.has_value());
  REQUIRE(*second == 2);
}
