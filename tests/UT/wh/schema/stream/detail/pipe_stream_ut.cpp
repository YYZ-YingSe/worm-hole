#include <exception>
#include <memory>
#include <stdexcept>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "helper/test_thread_wait.hpp"
#include "wh/core/bounded_queue.hpp"
#include "wh/schema/stream/detail/pipe_stream.hpp"

TEST_CASE(
    "pipe stream detail maps queue status and retries busy helper paths",
    "[UT][wh/schema/stream/detail/pipe_stream.hpp][map_pipe_queue_status][condition][branch]") {
  using wh::core::bounded_queue_status;
  using wh::schema::stream::detail::map_pipe_queue_status;

  REQUIRE(map_pipe_queue_status(bounded_queue_status::success) == wh::core::errc::ok);
  REQUIRE(map_pipe_queue_status(bounded_queue_status::empty) == wh::core::errc::queue_empty);
  REQUIRE(map_pipe_queue_status(bounded_queue_status::full) == wh::core::errc::queue_full);
  REQUIRE(map_pipe_queue_status(bounded_queue_status::closed) == wh::core::errc::channel_closed);
  REQUIRE(map_pipe_queue_status(bounded_queue_status::busy) == wh::core::errc::unavailable);
  REQUIRE(map_pipe_queue_status(bounded_queue_status::busy_async) == wh::core::errc::unavailable);

  int busy_attempts = 0;
  auto status = wh::schema::stream::detail::retry_busy_status([&]() noexcept {
    if (busy_attempts++ < 2) {
      return bounded_queue_status::busy;
    }
    return bounded_queue_status::full;
  });
  REQUIRE(status == bounded_queue_status::full);
  REQUIRE(busy_attempts == 3);

  int result_attempts = 0;
  auto retried = wh::schema::stream::detail::retry_busy_result([&]() noexcept {
    ++result_attempts;
    if (result_attempts < 3) {
      return wh::core::result<int, bounded_queue_status>::failure(bounded_queue_status::busy);
    }
    return wh::core::result<int, bounded_queue_status>{9};
  });
  REQUIRE(retried.has_value());
  REQUIRE(retried.value() == 9);
  REQUIRE(result_attempts == 3);
}

TEST_CASE(
    "pipe stream detail shared closed state normalization and exception forwarding cover success "
    "and terminal branches",
    "[UT][wh/schema/stream/detail/pipe_stream.hpp][normalize_pipe_read_sender][branch][boundary]") {
  using state_t = wh::schema::stream::detail::pipe_stream_state<int>;
  using chunk_t = wh::schema::stream::stream_chunk<int>;
  using result_t = wh::schema::stream::stream_result<chunk_t>;

  const auto &closed_a = wh::schema::stream::detail::shared_closed_pipe_state<state_t>();
  const auto &closed_b = wh::schema::stream::detail::shared_closed_pipe_state<state_t>();
  REQUIRE(closed_a.get() == closed_b.get());
  REQUIRE(closed_a->queue.is_closed());

  auto success_write = wh::testing::helper::wait_value_on_test_thread(
      wh::schema::stream::detail::normalize_pipe_write_sender(
          stdexec::just(), std::make_shared<state_t>(1U), false, false));
  REQUIRE(success_write.has_value());

  auto missing_write = wh::testing::helper::wait_value_on_test_thread(
      wh::schema::stream::detail::normalize_pipe_write_sender(
          stdexec::just(), std::shared_ptr<state_t>{}, true, false));
  REQUIRE(missing_write.has_error());
  REQUIRE(missing_write.error() == wh::core::errc::not_found);

  auto closed_write = wh::testing::helper::wait_value_on_test_thread(
      wh::schema::stream::detail::normalize_pipe_write_sender(
          stdexec::just_error(wh::core::bounded_queue_status::closed),
          std::make_shared<state_t>(1U), false, false));
  REQUIRE(closed_write.has_error());
  REQUIRE(closed_write.error() == wh::core::errc::channel_closed);

  auto read_value = wh::testing::helper::wait_value_on_test_thread(
      wh::schema::stream::detail::normalize_pipe_read_sender<int>(
          stdexec::just(7), std::make_shared<state_t>(1U), false, false));
  REQUIRE(read_value.has_value());
  REQUIRE(read_value.value().value == std::optional<int>{7});

  auto read_missing = wh::testing::helper::wait_value_on_test_thread(
      wh::schema::stream::detail::normalize_pipe_read_sender<int>(
          stdexec::just(7), std::shared_ptr<state_t>{}, true, false));
  REQUIRE(read_missing.has_error());
  REQUIRE(read_missing.error() == wh::core::errc::not_found);

  auto read_closed = wh::testing::helper::wait_value_on_test_thread(
      wh::schema::stream::detail::normalize_pipe_read_sender<int>(
          stdexec::just_error(wh::core::bounded_queue_status::closed),
          std::make_shared<state_t>(1U), false, false));
  REQUIRE(read_closed.has_value());
  REQUIRE(read_closed.value().is_terminal_eof());

  REQUIRE_THROWS_AS(wh::schema::stream::detail::rethrow_pipe_exception<result_t>(
                        std::make_exception_ptr(std::runtime_error{"boom"})),
                    std::runtime_error);
}

TEST_CASE("pipe stream detail normalization preserves real bounded-queue async read senders",
          "[UT][wh/schema/stream/detail/"
          "pipe_stream.hpp][normalize_pipe_read_sender][concurrency][boundary]") {
  using state_t = wh::schema::stream::detail::pipe_stream_state<int>;

  auto state = std::make_shared<state_t>(1U);
  REQUIRE(state->queue.try_push(7) == wh::core::bounded_queue_status::success);

  auto value = wh::testing::helper::wait_value_on_test_thread(
      wh::schema::stream::detail::normalize_pipe_read_sender<int>(state->queue.async_pop(), state,
                                                                  false, false));
  REQUIRE(value.has_value());
  REQUIRE(value.value().value == std::optional<int>{7});

  auto missing = wh::testing::helper::wait_value_on_test_thread(
      wh::schema::stream::detail::normalize_pipe_read_sender<int>(
          wh::schema::stream::detail::shared_closed_pipe_state<state_t>()->queue.async_pop(),
          std::shared_ptr<state_t>{}, true, false));
  REQUIRE(missing.has_error());
  REQUIRE(missing.error() == wh::core::errc::not_found);
}
