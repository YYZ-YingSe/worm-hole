#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <stop_token>

#include <stdexec/execution.hpp>

#include "helper/manual_scheduler.hpp"
#include "helper/sender_capture.hpp"
#include "helper/sender_env.hpp"
#include "wh/sync/async_mutex.hpp"

namespace {

struct would_block {};

using mutex_t = wh::sync::async_mutex;
using lock_guard_t = mutex_t::lock_guard;
using scheduler_t = wh::testing::helper::manual_scheduler<would_block>;
using scheduler_env_t =
    wh::testing::helper::scheduler_env<scheduler_t,
                                       wh::testing::helper::stop_token>;

} // namespace

TEST_CASE("async mutex public facade transfers the lock to queued waiters in FIFO order",
          "[core][sync][functional][concurrency]") {
  mutex_t mutex{};
  wh::testing::helper::manual_scheduler_state holder_scheduler{};
  wh::testing::helper::manual_scheduler_state first_waiter_scheduler{};
  wh::testing::helper::manual_scheduler_state second_waiter_scheduler{};

  wh::testing::helper::sender_capture<lock_guard_t> holder_capture{};
  auto holder_operation = stdexec::connect(
      mutex.lock(),
      wh::testing::helper::sender_capture_receiver<lock_guard_t, scheduler_env_t>{
          &holder_capture, {{&holder_scheduler}, {}}});
  stdexec::start(holder_operation);
  holder_scheduler.run_all();
  REQUIRE(holder_capture.ready.try_acquire());
  REQUIRE(holder_capture.terminal ==
          wh::testing::helper::sender_terminal_kind::value);
  auto holder_guard = std::move(holder_capture.value);
  REQUIRE(holder_guard.has_value());

  wh::testing::helper::sender_capture<lock_guard_t> first_waiter_capture{};
  auto first_waiter_operation = stdexec::connect(
      mutex.lock(),
      wh::testing::helper::sender_capture_receiver<lock_guard_t, scheduler_env_t>{
          &first_waiter_capture, {{&first_waiter_scheduler}, {}}});
  stdexec::start(first_waiter_operation);
  REQUIRE_FALSE(first_waiter_capture.ready.try_acquire());

  wh::testing::helper::sender_capture<lock_guard_t> second_waiter_capture{};
  auto second_waiter_operation = stdexec::connect(
      mutex.lock(),
      wh::testing::helper::sender_capture_receiver<lock_guard_t, scheduler_env_t>{
          &second_waiter_capture, {{&second_waiter_scheduler}, {}}});
  stdexec::start(second_waiter_operation);
  REQUIRE_FALSE(second_waiter_capture.ready.try_acquire());

  holder_guard.reset();
  first_waiter_scheduler.run_all();
  REQUIRE(first_waiter_capture.ready.try_acquire());
  REQUIRE(first_waiter_capture.terminal ==
          wh::testing::helper::sender_terminal_kind::value);
  REQUIRE_FALSE(second_waiter_capture.ready.try_acquire());

  auto first_waiter_guard = std::move(first_waiter_capture.value);
  REQUIRE(first_waiter_guard.has_value());
  first_waiter_guard.reset();

  second_waiter_scheduler.run_all();
  REQUIRE(second_waiter_capture.ready.try_acquire());
  REQUIRE(second_waiter_capture.terminal ==
          wh::testing::helper::sender_terminal_kind::value);
  auto second_waiter_guard = std::move(second_waiter_capture.value);
  REQUIRE(second_waiter_guard.has_value());
}

TEST_CASE("async mutex public facade stops one pending waiter and keeps the mutex reusable",
          "[core][sync][functional][concurrency][boundary]") {
  mutex_t mutex{};
  auto holder = mutex.try_lock();
  REQUIRE(holder.has_value());

  wh::testing::helper::manual_scheduler_state stop_scheduler{};
  wh::testing::helper::stop_source stop_source{};
  wh::testing::helper::sender_capture<lock_guard_t> stopped_capture{};
  auto stopped_operation = stdexec::connect(
      mutex.lock(),
      wh::testing::helper::sender_capture_receiver<lock_guard_t, scheduler_env_t>{
          &stopped_capture,
          {{&stop_scheduler}, stop_source.get_token()}});
  stdexec::start(stopped_operation);
  REQUIRE_FALSE(stopped_capture.ready.try_acquire());

  stop_source.request_stop();
  stop_scheduler.run_all();
  REQUIRE(stopped_capture.ready.try_acquire());
  REQUIRE(stopped_capture.terminal ==
          wh::testing::helper::sender_terminal_kind::stopped);

  holder.reset();
  REQUIRE(mutex.try_lock().has_value());
}
