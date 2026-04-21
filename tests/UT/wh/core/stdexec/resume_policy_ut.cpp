#include <exception>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "helper/manual_scheduler.hpp"
#include "helper/sender_capture.hpp"
#include "helper/test_thread_wait.hpp"
#include "wh/core/stdexec/resume_policy.hpp"

namespace {

using scheduler_t = wh::testing::helper::manual_scheduler<std::exception_ptr>;

} // namespace

TEST_CASE("resume policy resume_if covers unchanged passthrough and restore scheduling",
          "[UT][wh/core/stdexec/resume_policy.hpp][resume_if][condition][branch]") {
  using wh::core::resume_mode;

  auto unchanged_sender = wh::core::detail::resume_if<resume_mode::unchanged>(
      stdexec::just(3), wh::core::detail::resume_passthrough);
  REQUIRE(wh::testing::helper::wait_value_on_test_thread(std::move(unchanged_sender)) == 3);

  wh::testing::helper::manual_scheduler_state state{};
  auto restored_sender =
      wh::core::detail::resume_if<resume_mode::restore>(stdexec::just(5), scheduler_t{&state});

  wh::testing::helper::sender_capture<int> capture{};
  auto operation = stdexec::connect(std::move(restored_sender),
                                    wh::testing::helper::sender_capture_receiver<int>{&capture});
  stdexec::start(operation);
  REQUIRE(state.pending_count() == 1U);
  REQUIRE(state.run_one());
  REQUIRE(capture.ready.try_acquire());
  REQUIRE(capture.terminal == wh::testing::helper::sender_terminal_kind::value);
  REQUIRE(capture.value.has_value());
  REQUIRE(*capture.value == 5);
}

TEST_CASE("resume policy defer_resume_sender reads scheduler only in restore mode",
          "[UT][wh/core/stdexec/resume_policy.hpp][defer_resume_sender][condition][branch]") {
  using wh::core::resume_mode;

  bool saw_passthrough = false;
  auto unchanged_sender = wh::core::detail::defer_resume_sender<resume_mode::unchanged>(
      [&saw_passthrough](auto scheduler_or_passthrough) {
        using param_t = std::remove_cvref_t<decltype(scheduler_or_passthrough)>;
        STATIC_REQUIRE(std::same_as<param_t, wh::core::detail::resume_passthrough_t>);
        saw_passthrough = true;
        return stdexec::just(7);
      });
  REQUIRE_FALSE(saw_passthrough);
  REQUIRE(wh::testing::helper::wait_value_on_test_thread(std::move(unchanged_sender)) == 7);
  REQUIRE(saw_passthrough);

  wh::testing::helper::manual_scheduler_state state{};
  scheduler_t scheduler{&state};
  const scheduler_t &scheduler_ref = scheduler;
  auto restore_sender = wh::core::detail::defer_resume_sender<resume_mode::restore>(
      [](scheduler_t restore_scheduler) {
        return stdexec::schedule(restore_scheduler) | stdexec::then([] { return 11; });
      });
  auto wrapped_sender =
      wh::core::detail::write_sender_scheduler(std::move(restore_sender), scheduler_ref);

  wh::testing::helper::sender_capture<int> capture{};
  auto operation = stdexec::connect(std::move(wrapped_sender),
                                    wh::testing::helper::sender_capture_receiver<int>{&capture});
  stdexec::start(operation);
  REQUIRE(state.pending_count() == 1U);
  REQUIRE(state.run_one());
  REQUIRE(capture.ready.try_acquire());
  REQUIRE(capture.terminal == wh::testing::helper::sender_terminal_kind::value);
  REQUIRE(capture.value.has_value());
  REQUIRE(*capture.value == 11);
}

TEST_CASE("resume policy unchanged mode ignores provided schedulers completely",
          "[UT][wh/core/stdexec/resume_policy.hpp][resume_if][condition][branch][boundary]") {
  wh::testing::helper::manual_scheduler_state state{};
  auto sender = wh::core::detail::resume_if<wh::core::resume_mode::unchanged>(stdexec::just(17),
                                                                              scheduler_t{&state});

  REQUIRE(wh::testing::helper::wait_value_on_test_thread(std::move(sender)) == 17);
  REQUIRE(state.pending_count() == 0U);
}
