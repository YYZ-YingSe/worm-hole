#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <optional>
#include <semaphore>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

#include "wh/async/completion_tokens.hpp"
#include "wh/core/type_utils.hpp"
#include "wh/scheduler/context_helper.hpp"
#include "wh/sync/channel.hpp"
#include "wh/sync/sender_notify.hpp"

namespace {

using channel_t = wh::core::channel<int>;
using context_t =
    decltype(wh::core::make_scheduler_context(stdexec::inline_scheduler{}));

static_assert(wh::core::is_sender_v<decltype(std::declval<channel_t &>().push(
                  std::declval<context_t>(), std::declval<int>(),
                  wh::core::use_sender))>);
static_assert(wh::core::is_sender_v<decltype(std::declval<channel_t &>().pop(
                  std::declval<context_t>(), wh::core::use_sender))>);
static_assert(wh::core::is_sender_v<decltype(std::declval<channel_t &>().push(
                  std::declval<context_t>(), std::declval<int>(),
                  wh::core::use_awaitable))>);
static_assert(wh::core::is_sender_v<decltype(std::declval<channel_t &>().pop(
                  std::declval<context_t>(), wh::core::use_awaitable))>);

template <typename value_t, typename sender_t>
[[nodiscard]] auto consume_sender(sender_t &&sender) -> value_t {
  auto sync_result = stdexec::sync_wait(std::forward<sender_t>(sender));
  REQUIRE(sync_result.has_value());
  return std::move(std::get<0>(sync_result.value()));
}

} // namespace

TEST_CASE("channel try api and close drain semantics",
          "[core][channel][condition]") {
  channel_t channel(4U);

  auto first_try_pop = channel.try_pop();
  REQUIRE(first_try_pop.has_error());
  REQUIRE(first_try_pop.error() == wh::core::errc::queue_empty);

  REQUIRE(channel.try_push(11).has_value());
  REQUIRE(channel.close());
  REQUIRE_FALSE(channel.close());

  auto drained = channel.try_pop();
  REQUIRE(drained.has_value());
  REQUIRE(drained.value() == 11);

  auto closed_pop = channel.try_pop();
  REQUIRE(closed_pop.has_error());
  REQUIRE(closed_pop.error() == wh::core::errc::channel_closed);

  auto closed_push = channel.try_push(17);
  REQUIRE(closed_push.has_error());
  REQUIRE(closed_push.error() == wh::core::errc::channel_closed);
}

TEST_CASE("channel split sender and receiver semantics",
          "[core][channel][branch]") {
  channel_t channel(8U);
  auto [tx, rx] = channel.split();

  REQUIRE(tx.try_push(21).has_value());

  auto popped = rx.try_pop();
  REQUIRE(popped.has_value());
  REQUIRE(popped.value() == 21);

  REQUIRE(tx.close());
  REQUIRE(rx.is_closed());

  auto closed_pop = rx.try_pop();
  REQUIRE(closed_pop.has_error());
  REQUIRE(closed_pop.error() == wh::core::errc::channel_closed);
}

TEST_CASE("channel close wakes blocked sender", "[core][channel][condition]") {
  channel_t channel(1U);
  stdexec::inline_scheduler scheduler;
  auto context = wh::core::make_scheduler_context(scheduler);

  REQUIRE(channel.try_push(1).has_value());
  auto [tx, rx] = channel.split();

  std::optional<wh::core::result<void>> push_status;
  std::thread push_thread([&] {
    push_status = consume_sender<wh::core::result<void>>(
        tx.push(context, 2, wh::core::use_sender));
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  REQUIRE(channel.close());
  push_thread.join();

  REQUIRE(push_status.has_value());
  REQUIRE(push_status->has_error());
  REQUIRE(push_status->error() == wh::core::errc::channel_closed);

  auto drained = rx.try_pop();
  REQUIRE(drained.has_value());
  REQUIRE(drained.value() == 1);
}

TEST_CASE("channel close wakes blocked receiver",
          "[core][channel][condition]") {
  channel_t channel(1U);
  stdexec::inline_scheduler scheduler;
  auto context = wh::core::make_scheduler_context(scheduler);

  auto [tx, rx] = channel.split();
  std::optional<wh::core::result<int>> pop_status;
  std::thread pop_thread([&] {
    pop_status = consume_sender<wh::core::result<int>>(
        rx.pop(context, wh::core::use_sender));
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  REQUIRE(tx.close());
  pop_thread.join();

  REQUIRE(pop_status.has_value());
  REQUIRE(pop_status->has_error());
  REQUIRE(pop_status->error() == wh::core::errc::channel_closed);
}

TEST_CASE("channel async pop drains buffered values after close",
          "[core][channel][branch]") {
  channel_t channel(8U);
  stdexec::inline_scheduler scheduler;
  auto context = wh::core::make_scheduler_context(scheduler);

  REQUIRE(channel.try_push(31).has_value());
  REQUIRE(channel.try_push(32).has_value());
  REQUIRE(channel.close());

  auto first = consume_sender<wh::core::result<int>>(
      channel.pop(context, wh::core::use_sender));
  REQUIRE(first.has_value());
  REQUIRE(first.value() == 31);

  auto second = consume_sender<wh::core::result<int>>(
      channel.pop(context, wh::core::use_sender));
  REQUIRE(second.has_value());
  REQUIRE(second.value() == 32);

  auto end = consume_sender<wh::core::result<int>>(
      channel.pop(context, wh::core::use_sender));
  REQUIRE(end.has_error());
  REQUIRE(end.error() == wh::core::errc::channel_closed);
}

TEST_CASE("channel completion token three-mode contracts",
          "[core][channel][condition]") {
  channel_t channel(4U);
  stdexec::inline_scheduler scheduler;
  auto context = wh::core::make_scheduler_context(scheduler);

  std::binary_semaphore push_done{0};
  std::optional<wh::core::result<void>> push_status;
  channel.push(context, 41,
               wh::core::use_callback([&](wh::core::result<void> status) {
                 push_status = std::move(status);
                 push_done.release();
               }));

  push_done.acquire();
  REQUIRE(push_status.has_value());
  REQUIRE(push_status->has_value());

  auto awaitable_pop = channel.pop(context, wh::core::use_awaitable);
  auto pop_status =
      consume_sender<wh::core::result<int>>(std::move(awaitable_pop));
  REQUIRE(pop_status.has_value());
  REQUIRE(pop_status.value() == 41);
}

TEST_CASE("sender_notify rejects stale turn registration",
          "[core][channel][sender_notify]") {
  wh::core::sender_notify notify{};
  std::atomic<std::uint64_t> turn{4U};
  bool invoked = false;

  wh::core::sender_notify::waiter waiter{};
  waiter.turn_ptr = &turn;
  waiter.expected_turn = 3U;
  waiter.owner = &invoked;
  waiter.notify = [](void *owner, wh::core::sender_notify::waiter *) noexcept {
    auto *flag = static_cast<bool *>(owner);
    *flag = true;
  };

  REQUIRE_FALSE(notify.arm(waiter));
  REQUIRE_FALSE(invoked);
}

TEST_CASE("sender_notify wakes waiter at expected turn",
          "[core][channel][sender_notify]") {
  wh::core::sender_notify notify{};
  std::atomic<std::uint64_t> turn{6U};
  std::atomic<bool> invoked{false};

  wh::core::sender_notify::waiter waiter{};
  waiter.turn_ptr = &turn;
  waiter.expected_turn = 7U;
  waiter.owner = &invoked;
  waiter.notify = [](void *owner, wh::core::sender_notify::waiter *) noexcept {
    auto *flag = static_cast<std::atomic<bool> *>(owner);
    flag->store(true, std::memory_order_release);
  };
  waiter.channel_hint = wh::core::sender_notify::suggest_channel_index(
      waiter.turn_ptr, waiter.expected_turn);

  REQUIRE(notify.arm(waiter));
  turn.store(7U, std::memory_order_release);
  notify.notify(&turn, 7U);
  REQUIRE(invoked.load(std::memory_order_acquire));
}
