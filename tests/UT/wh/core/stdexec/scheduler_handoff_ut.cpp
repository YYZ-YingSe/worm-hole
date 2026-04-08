#include <catch2/catch_test_macros.hpp>

#include <exception>

#include <stdexec/execution.hpp>

#include "helper/manual_scheduler.hpp"
#include "helper/sender_capture.hpp"
#include "wh/core/stdexec/scheduler_handoff.hpp"

namespace {

using plain_scheduler_t = wh::testing::helper::manual_scheduler<void>;

struct would_block final {};
using try_scheduler_t = wh::testing::helper::manual_scheduler<would_block>;

} // namespace

TEST_CASE("scheduler handoff same_scheduler query and sender-like concepts cover both sender families",
          "[UT][wh/core/stdexec/scheduler_handoff.hpp][same_scheduler][condition][branch]") {
  wh::testing::helper::manual_scheduler_state same_state{};
  same_state.same_scheduler = true;
  wh::testing::helper::manual_scheduler_state different_state{};

  const auto same_scheduler = plain_scheduler_t{&same_state};
  const auto different_scheduler = plain_scheduler_t{&different_state};

  REQUIRE(wh::core::detail::scheduler_handoff::same_scheduler(same_scheduler));
  REQUIRE_FALSE(
      wh::core::detail::scheduler_handoff::same_scheduler(different_scheduler));
  REQUIRE_FALSE(
      wh::core::detail::scheduler_handoff::same_scheduler(stdexec::inline_scheduler{}));

  using schedule_sender_t = decltype(
      wh::core::detail::scheduler_handoff::make_schedule_handoff_sender(
          same_scheduler));
  using try_sender_t = decltype(
      wh::core::detail::scheduler_handoff::make_try_schedule_handoff_sender(
          try_scheduler_t{}));

  STATIC_REQUIRE(
      wh::core::detail::scheduler_handoff::schedule_handoff_sender_like<
          schedule_sender_t>);
  STATIC_REQUIRE(
      wh::core::detail::scheduler_handoff::try_schedule_handoff_sender_like<
          try_sender_t>);
}

TEST_CASE("scheduler handoff schedule sender uses target scheduler and env projection",
          "[UT][wh/core/stdexec/scheduler_handoff.hpp][make_schedule_handoff_sender][branch]") {
  wh::testing::helper::manual_scheduler_state state{};
  const auto scheduler = plain_scheduler_t{&state};
  auto sender =
      wh::core::detail::scheduler_handoff::make_schedule_handoff_sender(scheduler);

  REQUIRE(sender.target_scheduler().state == &state);
  REQUIRE(stdexec::get_completion_scheduler<stdexec::set_value_t>(sender.get_env()).state ==
          &state);

  wh::testing::helper::sender_capture<void> capture{};
  auto operation = stdexec::connect(
      std::move(sender),
      wh::testing::helper::sender_capture_receiver<void>{&capture});
  stdexec::start(operation);
  REQUIRE(state.pending_count() == 1U);
  REQUIRE(state.run_one());
  REQUIRE(capture.ready.try_acquire());
  REQUIRE(capture.terminal == wh::testing::helper::sender_terminal_kind::value);
}

TEST_CASE("scheduler handoff try-schedule sender covers success inline and error branches",
          "[UT][wh/core/stdexec/scheduler_handoff.hpp][make_try_schedule_handoff_sender][condition][branch][boundary]") {
  wh::testing::helper::manual_scheduler_state state{};
  const auto scheduler = try_scheduler_t{&state};
  auto sender =
      wh::core::detail::scheduler_handoff::make_try_schedule_handoff_sender(
          scheduler);

  REQUIRE(sender.target_scheduler().state == &state);

  wh::testing::helper::sender_capture<void> value_capture{};
  auto value_operation = stdexec::connect(
      sender, wh::testing::helper::sender_capture_receiver<void>{&value_capture});
  stdexec::start(value_operation);
  REQUIRE(state.pending_count() == 1U);
  REQUIRE(state.run_one());
  REQUIRE(value_capture.ready.try_acquire());
  REQUIRE(value_capture.terminal ==
          wh::testing::helper::sender_terminal_kind::value);

  state.inline_try_schedule = true;
  wh::testing::helper::sender_capture<void> inline_capture{};
  auto inline_operation = stdexec::connect(
      sender, wh::testing::helper::sender_capture_receiver<void>{&inline_capture});
  stdexec::start(inline_operation);
  REQUIRE(inline_capture.ready.try_acquire());
  REQUIRE(inline_capture.terminal ==
          wh::testing::helper::sender_terminal_kind::value);
  REQUIRE(state.inline_try_schedule_calls == 1U);

  state.inline_try_schedule = false;
  state.allow_try_schedule = false;
  wh::testing::helper::sender_capture<void> error_capture{};
  auto error_operation = stdexec::connect(
      sender, wh::testing::helper::sender_capture_receiver<void>{&error_capture});
  stdexec::start(error_operation);
  REQUIRE(error_capture.ready.try_acquire());
  REQUIRE(error_capture.terminal ==
          wh::testing::helper::sender_terminal_kind::error);
  REQUIRE(error_capture.error != nullptr);
}
