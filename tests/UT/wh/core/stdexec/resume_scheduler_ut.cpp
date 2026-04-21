#include <catch2/catch_test_macros.hpp>
#include <exec/completion_behavior.hpp>
#include <stdexec/execution.hpp>

#include "helper/manual_scheduler.hpp"
#include "helper/sender_capture.hpp"
#include "helper/test_thread_wait.hpp"
#include "wh/core/stdexec/resume_scheduler.hpp"

namespace {

using scheduler_t = wh::testing::helper::manual_scheduler<std::exception_ptr>;

struct custom_query_t {
  template <typename env_t>
  [[nodiscard]] auto operator()(const env_t &env) const noexcept(noexcept(env.query(*this)))
      -> decltype(env.query(*this)) {
    return env.query(*this);
  }
};

inline constexpr custom_query_t custom_query{};

struct scheduler_only_env {
  scheduler_t scheduler{};
  int token{0};

  [[nodiscard]] auto query(stdexec::get_scheduler_t) const noexcept -> scheduler_t {
    return scheduler;
  }

  [[nodiscard]] auto query(custom_query_t) const noexcept -> int { return token; }
};

struct completion_first_env {
  scheduler_t completion_scheduler{};
  scheduler_t launch_scheduler{};
  int token{0};

  template <typename cpo_t>
  [[nodiscard]] auto query(stdexec::get_completion_scheduler_t<cpo_t>) const noexcept
      -> scheduler_t {
    return completion_scheduler;
  }

  [[nodiscard]] auto query(stdexec::get_scheduler_t) const noexcept -> scheduler_t {
    return launch_scheduler;
  }

  [[nodiscard]] auto query(custom_query_t) const noexcept -> int { return token; }
};

} // namespace

TEST_CASE(
    "resume scheduler selection prefers completion scheduler and launch selection prefers "
    "scheduler",
    "[UT][wh/core/stdexec/resume_scheduler.hpp][select_resume_scheduler][condition][branch]") {
  wh::testing::helper::manual_scheduler_state completion_state{};
  wh::testing::helper::manual_scheduler_state launch_state{};

  const completion_first_env env{
      .completion_scheduler = scheduler_t{&completion_state},
      .launch_scheduler = scheduler_t{&launch_state},
      .token = 9,
  };

  STATIC_REQUIRE(
      wh::core::detail::env_with_resume_scheduler<stdexec::set_value_t, completion_first_env>);
  STATIC_REQUIRE(wh::core::detail::env_with_launch_scheduler<completion_first_env>);
  STATIC_REQUIRE(
      std::same_as<
          wh::core::detail::selected_resume_scheduler_t<stdexec::set_value_t, completion_first_env>,
          scheduler_t>);
  STATIC_REQUIRE(std::same_as<wh::core::detail::selected_launch_scheduler_t<completion_first_env>,
                              scheduler_t>);

  const auto resume = wh::core::detail::select_resume_scheduler<stdexec::set_value_t>(env);
  const auto launch = wh::core::detail::select_launch_scheduler(env);
  REQUIRE(resume.state == &completion_state);
  REQUIRE(launch.state == &launch_state);
  REQUIRE(wh::core::detail::get_resume_scheduler(env).state == &completion_state);
  REQUIRE(wh::core::detail::get_launch_scheduler(env).state == &launch_state);
}

TEST_CASE("resume scheduler helpers expose scheduler-only env and scheduler wrappers",
          "[UT][wh/core/stdexec/resume_scheduler.hpp][make_scheduler_env][branch][boundary]") {
  wh::testing::helper::manual_scheduler_state state{};
  const scheduler_t scheduler{&state};
  const scheduler_only_env outer{.scheduler = scheduler, .token = 17};
  using receiver_t =
      wh::testing::helper::sender_capture_receiver<int,
                                                   wh::testing::helper::scheduler_env<scheduler_t>>;

  STATIC_REQUIRE(wh::core::detail::receiver_with_resume_scheduler<receiver_t>);
  STATIC_REQUIRE(wh::core::detail::receiver_with_launch_scheduler<receiver_t>);
  STATIC_REQUIRE(wh::core::detail::scheduler_query_v<stdexec::get_scheduler_t>);
  STATIC_REQUIRE(wh::core::detail::scheduler_query_v<
                 stdexec::get_completion_scheduler_t<stdexec::set_value_t>>);
  STATIC_REQUIRE(
      wh::core::detail::scheduler_query_v<stdexec::get_completion_domain_t<stdexec::set_value_t>>);

  const auto query_env = wh::core::detail::make_scheduler_queries(scheduler);
  REQUIRE(stdexec::get_scheduler(query_env).state == &state);
  REQUIRE(stdexec::get_delegation_scheduler(query_env).state == &state);

  const auto wrapped_env = wh::core::detail::make_scheduler_env(outer, scheduler);
  REQUIRE(stdexec::get_scheduler(wrapped_env).state == &state);
  REQUIRE(stdexec::get_delegation_scheduler(wrapped_env).state == &state);
  REQUIRE(custom_query(wrapped_env) == 17);

  auto written_sender = wh::core::detail::write_sender_scheduler(
      stdexec::read_env(stdexec::get_scheduler), scheduler);
  const auto written_scheduler =
      wh::testing::helper::wait_value_on_test_thread(std::move(written_sender));
  REQUIRE(written_scheduler.state == &state);

  auto read_sender = wh::core::read_resume_scheduler(
      [](auto selected_scheduler) { return stdexec::just(selected_scheduler); });
  auto wrapped_sender = wh::core::detail::write_sender_scheduler(std::move(read_sender), scheduler);
  const auto read_scheduler =
      wh::testing::helper::wait_value_on_test_thread(std::move(wrapped_sender));
  REQUIRE(read_scheduler.state == &state);

  auto resumed = wh::core::resume_on(stdexec::just(5), scheduler);
  wh::testing::helper::sender_capture<int> capture{};
  auto operation = stdexec::connect(std::move(resumed),
                                    wh::testing::helper::sender_capture_receiver<int>{&capture});
  stdexec::start(operation);
  REQUIRE(state.pending_count() == 1U);
  REQUIRE(state.run_one());
  REQUIRE(capture.ready.try_acquire());
  REQUIRE(capture.terminal == wh::testing::helper::sender_terminal_kind::value);
  REQUIRE(capture.value.has_value());
  REQUIRE(*capture.value == 5);
}

TEST_CASE("resume scheduler async completion env and erasure keep scheduler access",
          "[UT][wh/core/stdexec/resume_scheduler.hpp][async_completion_env][condition][branch]") {
  wh::testing::helper::manual_scheduler_state state{};
  const scheduler_t scheduler{&state};
  const scheduler_only_env outer{.scheduler = scheduler, .token = 23};
  const wh::core::detail::async_completion_env async_env{};

  REQUIRE(async_env.query(exec::get_completion_behavior_t<stdexec::set_value_t>{}) ==
          exec::completion_behavior::asynchronous_affine);
  REQUIRE(async_env.query(exec::get_completion_behavior_t<stdexec::set_error_t>{}) ==
          exec::completion_behavior::asynchronous_affine);
  REQUIRE(async_env.query(exec::get_completion_behavior_t<stdexec::set_stopped_t>{}) ==
          exec::completion_behavior::asynchronous_affine);

  const auto queried_scheduler =
      async_env.query(stdexec::get_completion_scheduler_t<stdexec::set_value_t>{}, outer);
  REQUIRE(queried_scheduler.state == &state);

  auto erased = wh::core::detail::erase_resume_scheduler(scheduler);
  wh::testing::helper::sender_capture<int> capture{};
  auto operation = stdexec::connect(stdexec::schedule(erased) | stdexec::then([] { return 1; }),
                                    wh::testing::helper::sender_capture_receiver<int>{&capture});
  stdexec::start(operation);
  REQUIRE(state.pending_count() == 1U);
  REQUIRE(state.run_one());
  REQUIRE(capture.ready.try_acquire());
  REQUIRE(capture.terminal == wh::testing::helper::sender_terminal_kind::value);
  REQUIRE(capture.value.has_value());
  REQUIRE(*capture.value == 1);

  auto erased_inline = wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{});
  auto erased_again = wh::core::detail::erase_resume_scheduler(std::move(erased_inline));
  const auto waited =
      stdexec::sync_wait(stdexec::schedule(erased_again) | stdexec::then([] { return 2; }));
  REQUIRE(waited.has_value());
  REQUIRE(std::get<0>(*waited) == 2);
}
