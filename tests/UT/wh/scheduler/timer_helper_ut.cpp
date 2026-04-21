#include <chrono>

#include <catch2/catch_test_macros.hpp>
#include <exec/timed_thread_scheduler.hpp>
#include <stdexec/execution.hpp>

#include "wh/scheduler/timer_helper.hpp"

TEST_CASE("timer helper exposes timed scheduler operations and timeout senders",
          "[UT][wh/scheduler/timer_helper.hpp][timeout][condition][branch][boundary]") {
  exec::timed_thread_context context{};
  const auto scheduler = context.get_scheduler();

  STATIC_REQUIRE(wh::core::timed_scheduler_like<decltype(scheduler)>);

  const auto now = wh::core::scheduler_now(scheduler);
  auto after =
      stdexec::sync_wait(wh::core::schedule_after(scheduler, std::chrono::milliseconds{1}));
  REQUIRE(after.has_value());

  auto at =
      stdexec::sync_wait(wh::core::schedule_at(scheduler, now + std::chrono::milliseconds{1}));
  REQUIRE(at.has_value());

  auto timeout_sender = wh::core::timeout<wh::core::result<int>>(
      scheduler, stdexec::just(wh::core::result<int>{7}), std::chrono::milliseconds{1});
  auto timeout_result = stdexec::sync_wait(std::move(timeout_sender));
  REQUIRE(timeout_result.has_value());
}

TEST_CASE("timer helper timeout helpers surface timeout branch for slow senders",
          "[UT][wh/scheduler/timer_helper.hpp][timeout_at][condition][branch][boundary]") {
  exec::timed_thread_context context{};
  const auto scheduler = context.get_scheduler();
  const auto now = wh::core::scheduler_now(scheduler);

  auto slow_sender = wh::core::schedule_after(scheduler, std::chrono::milliseconds{10}) |
                     stdexec::then([]() noexcept { return wh::core::result<int>{1}; });

  auto timed_out = wh::core::timeout<wh::core::result<int>>(scheduler, std::move(slow_sender),
                                                            std::chrono::milliseconds{1});
  auto timed_out_result = stdexec::sync_wait(std::move(timed_out));
  REQUIRE(timed_out_result.has_value());
  REQUIRE(std::get<0>(*timed_out_result).has_error());
  REQUIRE(std::get<0>(*timed_out_result).error() == wh::core::errc::timeout);

  auto slow_sender_at = wh::core::schedule_after(scheduler, std::chrono::milliseconds{10}) |
                        stdexec::then([]() noexcept { return wh::core::result<int>{2}; });
  auto timed_out_at = wh::core::timeout_at<wh::core::result<int>>(
      scheduler, std::move(slow_sender_at), now + std::chrono::milliseconds{1});
  auto timed_out_at_result = stdexec::sync_wait(std::move(timed_out_at));
  REQUIRE(timed_out_at_result.has_value());
  REQUIRE(std::get<0>(*timed_out_at_result).has_error());
  REQUIRE(std::get<0>(*timed_out_at_result).error() == wh::core::errc::timeout);
}
