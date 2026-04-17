#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/core/intrusive_ptr.hpp"
#include "helper/manual_scheduler.hpp"
#include "helper/sender_capture.hpp"
#include "helper/sender_env.hpp"
#include "wh/core/cursor_reader/detail/read_sender.hpp"

namespace {

using result_t = wh::core::result<int>;

struct sender_source_stats {
  std::vector<std::optional<result_t>> try_results{};
  std::size_t try_index{0U};
  result_t async_result{0};
};

struct sender_async_source {
  std::shared_ptr<sender_source_stats> stats{};

  [[nodiscard]] auto read() -> result_t { return result_t{0}; }

  [[nodiscard]] auto try_read() -> std::optional<result_t> {
    if (stats->try_index < stats->try_results.size()) {
      return stats->try_results[stats->try_index++];
    }
    return std::nullopt;
  }

  [[nodiscard]] auto read_async() { return stdexec::just(stats->async_result); }

  [[nodiscard]] auto close() -> wh::core::result<void> { return {}; }
};

using scheduler_t = wh::testing::helper::manual_scheduler<void>;
using env_t = wh::testing::helper::scheduler_env<scheduler_t, std::stop_token>;
using capture_t = wh::testing::helper::sender_capture<result_t>;
using receiver_t =
    wh::testing::helper::sender_capture_receiver<result_t, env_t>;
using policy_t =
    wh::core::cursor_reader_detail::default_policy<sender_async_source>;
using sender_t = wh::core::cursor_reader_detail::read_sender<sender_async_source, policy_t>;

} // namespace

TEST_CASE("read sender returns internal and closed results for null and released states",
          "[UT][wh/core/cursor_reader/detail/read_sender.hpp][read_sender::connect][branch][boundary]") {
  wh::testing::helper::manual_scheduler_state scheduler_state{};
  scheduler_t scheduler{&scheduler_state};
  env_t env{scheduler, {}};

  capture_t missing_capture{};
  auto missing_operation = stdexec::connect(
      sender_t{.state_ = nullptr, .reader_index = 0U, .released = false},
      receiver_t{&missing_capture, env});
  stdexec::start(missing_operation);
  REQUIRE(missing_capture.ready.try_acquire());
  REQUIRE(missing_capture.value.has_value());
  REQUIRE(missing_capture.value->has_error());
  REQUIRE(missing_capture.value->error() == wh::core::errc::internal_error);

  auto stats = std::make_shared<sender_source_stats>();
  auto state = wh::core::detail::make_intrusive<
      wh::core::cursor_reader_detail::shared_state<sender_async_source, policy_t>>(
      sender_async_source{stats}, 1U);

  capture_t released_capture{};
  auto released_operation = stdexec::connect(
      sender_t{.state_ = state, .reader_index = 0U, .released = true},
      receiver_t{&released_capture, env});
  stdexec::start(released_operation);
  REQUIRE(released_capture.ready.try_acquire());
  REQUIRE(released_capture.value.has_value());
  REQUIRE(released_capture.value->has_error());
  REQUIRE(released_capture.value->error() == wh::core::errc::channel_closed);
}

TEST_CASE("read sender honors pre-requested stop before registering waiter",
          "[UT][wh/core/cursor_reader/detail/read_sender.hpp][read_operation::start][condition][branch]") {
  wh::testing::helper::manual_scheduler_state scheduler_state{};
  scheduler_t scheduler{&scheduler_state};
  std::stop_source stop_source{};
  stop_source.request_stop();
  env_t env{scheduler, stop_source.get_token()};

  auto stats = std::make_shared<sender_source_stats>();
  stats->try_results = {std::nullopt};
  auto state = wh::core::detail::make_intrusive<
      wh::core::cursor_reader_detail::shared_state<sender_async_source, policy_t>>(
      sender_async_source{stats}, 1U);

  capture_t capture{};
  auto operation = stdexec::connect(
      sender_t{.state_ = state, .reader_index = 0U, .released = false},
      receiver_t{&capture, env});
  stdexec::start(operation);

  REQUIRE(capture.ready.try_acquire());
  REQUIRE(capture.terminal == wh::testing::helper::sender_terminal_kind::stopped);
  REQUIRE(scheduler_state.pending_count() == 0U);
}

TEST_CASE("read sender prebuffered ready path schedules handoff delivery on non-matching scheduler",
          "[UT][wh/core/cursor_reader/detail/read_sender.hpp][read_operation::begin_completion][branch]") {
  auto handoff_stats = std::make_shared<sender_source_stats>();
  handoff_stats->try_results = {std::optional<result_t>{result_t{9}}};

  wh::testing::helper::manual_scheduler_state handoff_scheduler_state{};
  scheduler_t handoff_scheduler{&handoff_scheduler_state};
  env_t handoff_env{handoff_scheduler, {}};
  auto handoff_state = wh::core::detail::make_intrusive<
      wh::core::cursor_reader_detail::shared_state<sender_async_source, policy_t>>(
      sender_async_source{handoff_stats}, 2U);
  auto primed_handoff = handoff_state->try_read_for(1U);
  REQUIRE(primed_handoff.has_value());
  REQUIRE(primed_handoff->has_value());
  REQUIRE(primed_handoff->value() == 9);

  capture_t handoff_capture{};
  auto handoff_operation = stdexec::connect(
      sender_t{.state_ = handoff_state, .reader_index = 0U, .released = false},
      receiver_t{&handoff_capture, handoff_env});
  stdexec::start(handoff_operation);
  REQUIRE_FALSE(handoff_capture.ready.try_acquire());
  REQUIRE(handoff_scheduler_state.pending_count() == 1U);
  REQUIRE(handoff_scheduler_state.run_one());
  REQUIRE(handoff_capture.ready.try_acquire());
  REQUIRE(handoff_capture.value.has_value());
  REQUIRE(handoff_capture.value->has_value());
  REQUIRE(handoff_capture.value->value() == 9);
}

TEST_CASE("read sender pending async path starts pull and hands off final completion",
          "[UT][wh/core/cursor_reader/detail/read_sender.hpp][read_operation::complete_ready][concurrency][branch]") {
  auto stats = std::make_shared<sender_source_stats>();
  stats->try_results = {std::nullopt};
  stats->async_result = result_t{13};

  wh::testing::helper::manual_scheduler_state scheduler_state{};
  scheduler_t scheduler{&scheduler_state};
  env_t env{scheduler, {}};
  auto state = wh::core::detail::make_intrusive<
      wh::core::cursor_reader_detail::shared_state<sender_async_source, policy_t>>(
      sender_async_source{stats}, 1U);

  capture_t capture{};
  auto operation = stdexec::connect(
      sender_t{.state_ = state, .reader_index = 0U, .released = false},
      receiver_t{&capture, env});
  stdexec::start(operation);

  REQUIRE_FALSE(capture.ready.try_acquire());
  REQUIRE(scheduler_state.pending_count() == 1U);
  REQUIRE(scheduler_state.run_one());
  REQUIRE_FALSE(capture.ready.try_acquire());
  REQUIRE(scheduler_state.pending_count() == 1U);
  REQUIRE(scheduler_state.run_one());
  REQUIRE(capture.ready.try_acquire());
  REQUIRE(capture.value.has_value());
  REQUIRE(capture.value->has_value());
  REQUIRE(capture.value->value() == 13);
}

TEST_CASE("read sender pending async path finishes on first scheduler turn when marked same-scheduler",
          "[UT][wh/core/cursor_reader/detail/read_sender.hpp][read_operation::finish][branch]") {
  auto stats = std::make_shared<sender_source_stats>();
  stats->try_results = {std::nullopt};
  stats->async_result = result_t{21};

  wh::testing::helper::manual_scheduler_state scheduler_state{};
  scheduler_state.same_scheduler = true;
  scheduler_t scheduler{&scheduler_state};
  env_t env{scheduler, {}};
  auto state = wh::core::detail::make_intrusive<
      wh::core::cursor_reader_detail::shared_state<sender_async_source, policy_t>>(
      sender_async_source{stats}, 1U);

  capture_t capture{};
  auto operation = stdexec::connect(
      sender_t{.state_ = state, .reader_index = 0U, .released = false},
      receiver_t{&capture, env});
  stdexec::start(operation);

  REQUIRE_FALSE(capture.ready.try_acquire());
  REQUIRE(scheduler_state.pending_count() == 1U);
  REQUIRE(scheduler_state.run_one());
  REQUIRE(capture.ready.try_acquire());
  REQUIRE(capture.value.has_value());
  REQUIRE(capture.value->has_value());
  REQUIRE(capture.value->value() == 21);
  REQUIRE(scheduler_state.pending_count() == 0U);
}
