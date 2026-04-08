#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

#include <stdexec/execution.hpp>

#include "helper/manual_scheduler.hpp"
#include "helper/sender_env.hpp"
#include "wh/sync/async_mutex.hpp"

namespace {

using mutex_t = wh::sync::async_mutex;
using lock_guard_t = mutex_t::lock_guard;

struct would_block {};
using manual_scheduler_state = wh::testing::helper::manual_scheduler_state;
using manual_scheduler = wh::testing::helper::manual_scheduler<would_block>;

template <typename scheduler_t>
using receiver_env_t =
    wh::testing::helper::scheduler_env<scheduler_t, std::stop_token>;
using receiver_env = receiver_env_t<manual_scheduler>;

template <typename scheduler_t>
using completion_receiver_env_t =
    wh::testing::helper::completion_scheduler_env<scheduler_t, std::stop_token>;
using completion_receiver_env = completion_receiver_env_t<manual_scheduler>;

struct receiver_state {
  bool value_called{false};
  bool stopped_called{false};
  std::optional<lock_guard_t> guard{};
};

struct lock_receiver {
  using receiver_concept = stdexec::receiver_t;

  receiver_state *state{nullptr};
  receiver_env env{};

  auto set_value(lock_guard_t guard) noexcept -> void {
    state->value_called = true;
    state->guard.emplace(std::move(guard));
  }
  template <typename error_t> auto set_error(error_t &&) noexcept -> void {}
  auto set_stopped() noexcept -> void { state->stopped_called = true; }
  [[nodiscard]] auto get_env() const noexcept -> receiver_env { return env; }
};

struct completion_lock_receiver {
  using receiver_concept = stdexec::receiver_t;

  receiver_state *state{nullptr};
  completion_receiver_env env{};

  auto set_value(lock_guard_t guard) noexcept -> void {
    state->value_called = true;
    state->guard.emplace(std::move(guard));
  }
  template <typename error_t> auto set_error(error_t &&) noexcept -> void {}
  auto set_stopped() noexcept -> void { state->stopped_called = true; }
  [[nodiscard]] auto get_env() const noexcept -> completion_receiver_env {
    return env;
  }
};

struct ordering_state {
  bool value_called{false};
  std::optional<lock_guard_t> guard{};
  std::vector<int> *order{nullptr};
  int id{0};
};

struct ordering_receiver {
  using receiver_concept = stdexec::receiver_t;
  ordering_state *state{nullptr};
  receiver_env env{};

  auto set_value(lock_guard_t guard) noexcept -> void {
    state->value_called = true;
    state->order->push_back(state->id);
    state->guard.emplace(std::move(guard));
  }
  template <typename error_t> auto set_error(error_t &&) noexcept -> void {}
  auto set_stopped() noexcept -> void {}
  [[nodiscard]] auto get_env() const noexcept -> receiver_env { return env; }
};

} // namespace

TEST_CASE("async mutex try_lock and lock_guard manage ownership",
          "[UT][wh/sync/async_mutex.hpp][async_mutex::try_lock][branch][boundary]") {
  mutex_t mutex{};
  auto first = mutex.try_lock();
  REQUIRE(first.has_value());
  REQUIRE(static_cast<bool>(*first));
  REQUIRE_FALSE(mutex.try_lock().has_value());

  lock_guard_t moved = std::move(*first);
  REQUIRE(static_cast<bool>(moved));
  REQUIRE_FALSE(static_cast<bool>(*first));
  moved.unlock();
  REQUIRE_FALSE(static_cast<bool>(moved));
  REQUIRE(mutex.try_lock().has_value());
}

TEST_CASE("async mutex lock completes on scheduler and unlocks contended waiters",
          "[UT][wh/sync/async_mutex.hpp][async_mutex::lock][branch][concurrency]") {
  mutex_t mutex{};
  manual_scheduler_state sched1{};
  manual_scheduler_state sched2{};

  receiver_state state1{};
  auto op1 = stdexec::connect(
      mutex.lock(), lock_receiver{&state1, receiver_env{manual_scheduler{&sched1}}});
  stdexec::start(op1);
  sched1.run_all();
  REQUIRE(state1.value_called);
  REQUIRE(state1.guard.has_value());

  receiver_state state2{};
  auto op2 = stdexec::connect(
      mutex.lock(), lock_receiver{&state2, receiver_env{manual_scheduler{&sched2}}});
  stdexec::start(op2);
  sched2.run_all();
  REQUIRE_FALSE(state2.value_called);

  state1.guard.reset();
  sched2.run_all();
  REQUIRE(state2.value_called);
  REQUIRE(state2.guard.has_value());
  state2.guard.reset();
}

TEST_CASE("async mutex lock honors completion scheduler and stop cancellation",
          "[UT][wh/sync/async_mutex.hpp][async_mutex::lock_sender][condition][branch]") {
  mutex_t mutex{};
  manual_scheduler_state completion_sched{};

  receiver_state completion_state{};
  auto completion_op = stdexec::connect(
      mutex.lock(),
      completion_lock_receiver{&completion_state,
                               completion_receiver_env{manual_scheduler{&completion_sched}}});
  stdexec::start(completion_op);
  REQUIRE_FALSE(completion_state.value_called);
  REQUIRE(completion_sched.pending_count() == 1U);
  completion_sched.run_all();
  REQUIRE(completion_state.value_called);
  completion_state.guard.reset();

  receiver_state stopped_state{};
  std::stop_source stop_source{};
  stop_source.request_stop();
  manual_scheduler_state stop_sched{};
  auto stopped_op = stdexec::connect(
      mutex.lock(), lock_receiver{&stopped_state,
                                  receiver_env{manual_scheduler{&stop_sched},
                                               stop_source.get_token()}});
  stdexec::start(stopped_op);
  stop_sched.run_all();
  REQUIRE(stopped_state.stopped_called);
  REQUIRE_FALSE(stopped_state.value_called);
  REQUIRE(mutex.try_lock().has_value());
}

TEST_CASE("async mutex preserves FIFO waiter ordering under contention",
          "[UT][wh/sync/async_mutex.hpp][async_mutex::lock_guard::unlock][concurrency]") {
  mutex_t mutex{};
  manual_scheduler_state holder_sched{};
  manual_scheduler_state sched_a{};
  manual_scheduler_state sched_b{};
  manual_scheduler_state sched_c{};

  receiver_state holder_state{};
  auto holder_op = stdexec::connect(
      mutex.lock(),
      lock_receiver{&holder_state, receiver_env{manual_scheduler{&holder_sched}}});
  stdexec::start(holder_op);
  holder_sched.run_all();
  REQUIRE(holder_state.value_called);

  std::vector<int> order{};
  ordering_state state_a{false, std::nullopt, &order, 1};
  ordering_state state_b{false, std::nullopt, &order, 2};
  ordering_state state_c{false, std::nullopt, &order, 3};

  auto op_a = stdexec::connect(
      mutex.lock(), ordering_receiver{&state_a, receiver_env{manual_scheduler{&sched_a}}});
  auto op_b = stdexec::connect(
      mutex.lock(), ordering_receiver{&state_b, receiver_env{manual_scheduler{&sched_b}}});
  auto op_c = stdexec::connect(
      mutex.lock(), ordering_receiver{&state_c, receiver_env{manual_scheduler{&sched_c}}});
  stdexec::start(op_a);
  stdexec::start(op_b);
  stdexec::start(op_c);

  holder_state.guard.reset();
  sched_a.run_all();
  REQUIRE(state_a.value_called);
  state_a.guard.reset();
  sched_b.run_all();
  REQUIRE(state_b.value_called);
  state_b.guard.reset();
  sched_c.run_all();
  REQUIRE(state_c.value_called);
  REQUIRE(order == std::vector<int>{1, 2, 3});
  state_c.guard.reset();
}
