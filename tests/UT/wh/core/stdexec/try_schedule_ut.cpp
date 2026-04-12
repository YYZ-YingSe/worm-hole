#include <catch2/catch_test_macros.hpp>

#include <type_traits>

#include <stdexec/execution.hpp>

#include "helper/manual_scheduler.hpp"
#include "helper/sender_capture.hpp"
#include "wh/core/stdexec/try_schedule.hpp"

namespace {

struct would_block final {};

} // namespace

TEST_CASE("try schedule cpo and concept cover success inline and error branches",
          "[UT][wh/core/stdexec/try_schedule.hpp][try_schedule][condition][branch][boundary]") {
  using try_scheduler_t = wh::testing::helper::manual_scheduler<would_block>;
  using plain_scheduler_t = wh::testing::helper::manual_scheduler<void>;

  STATIC_REQUIRE(wh::core::detail::try_schedule_member_callable<try_scheduler_t>);
  STATIC_REQUIRE(wh::core::try_scheduler<try_scheduler_t>);
  STATIC_REQUIRE_FALSE(wh::core::try_scheduler<plain_scheduler_t>);
  STATIC_REQUIRE(std::same_as<wh::core::try_schedule_result_t<try_scheduler_t>,
                              typename try_scheduler_t::try_schedule_sender>);

  wh::testing::helper::manual_scheduler_state state{};
  const try_scheduler_t scheduler{&state};

  wh::testing::helper::sender_capture<void> value_capture{};
  auto value_op = stdexec::connect(
      wh::core::try_schedule(scheduler),
      wh::testing::helper::sender_capture_receiver<void>{&value_capture});
  stdexec::start(value_op);
  REQUIRE(state.pending_count() == 1U);
  REQUIRE(state.run_one());
  REQUIRE(value_capture.ready.try_acquire());
  REQUIRE(value_capture.terminal == wh::testing::helper::sender_terminal_kind::value);

  state.inline_try_schedule = true;
  wh::testing::helper::sender_capture<void> inline_capture{};
  auto inline_op = stdexec::connect(
      wh::core::try_schedule(scheduler),
      wh::testing::helper::sender_capture_receiver<void>{&inline_capture});
  stdexec::start(inline_op);
  REQUIRE(inline_capture.ready.try_acquire());
  REQUIRE(inline_capture.terminal ==
          wh::testing::helper::sender_terminal_kind::value);
  REQUIRE(state.pending_count() == 0U);
  REQUIRE(state.inline_try_schedule_calls == 1U);

  state.inline_try_schedule = false;
  state.allow_try_schedule = false;
  wh::testing::helper::sender_capture<void> error_capture{};
  auto error_op = stdexec::connect(
      wh::core::try_schedule(scheduler),
      wh::testing::helper::sender_capture_receiver<void>{&error_capture});
  stdexec::start(error_op);
  REQUIRE(error_capture.ready.try_acquire());
  REQUIRE(error_capture.terminal == wh::testing::helper::sender_terminal_kind::error);
  REQUIRE(error_capture.error != nullptr);
}

TEST_CASE("try schedule result aliases remain stable for reference-qualified schedulers",
          "[UT][wh/core/stdexec/try_schedule.hpp][try_schedule_result_t][condition][branch][boundary]") {
  using try_scheduler_t = wh::testing::helper::manual_scheduler<would_block>;
  STATIC_REQUIRE(std::same_as<wh::core::try_schedule_result_t<const try_scheduler_t &>,
                              typename try_scheduler_t::try_schedule_sender>);

  wh::testing::helper::manual_scheduler_state state{};
  const try_scheduler_t scheduler{&state};
  auto sender = wh::core::try_schedule(scheduler);

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
