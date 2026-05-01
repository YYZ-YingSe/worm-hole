#include <chrono>
#include <optional>
#include <stop_token>
#include <thread>
#include <tuple>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "helper/sender_capture.hpp"
#include "helper/sender_env.hpp"
#include "helper/static_thread_scheduler.hpp"
#include "wh/core/cursor_reader.hpp"
#include "wh/schema/stream/pipe.hpp"

TEST_CASE("cursor reader public facade retains one source across fixed readers",
          "[core][cursor_reader][functional][concurrency]") {
  auto [writer, source] = wh::schema::stream::make_pipe_stream<int>(8U);
  REQUIRE(writer.try_write(1).has_value());
  REQUIRE(writer.try_write(2).has_value());
  REQUIRE(writer.try_write(3).has_value());
  REQUIRE(writer.close().has_value());

  auto readers = wh::core::make_cursor_readers(std::move(source), 2U);
  REQUIRE(readers.size() == 2U);

  const auto left_1 = readers[0].read();
  const auto right_1 = readers[1].read();
  REQUIRE(left_1.has_value());
  REQUIRE(right_1.has_value());
  REQUIRE(left_1.value().value == std::optional<int>{1});
  REQUIRE(right_1.value().value == std::optional<int>{1});

  const auto right_2 = readers[1].read();
  const auto left_2 = readers[0].read();
  REQUIRE(left_2.has_value());
  REQUIRE(right_2.has_value());
  REQUIRE(left_2.value().value == std::optional<int>{2});
  REQUIRE(right_2.value().value == std::optional<int>{2});

  const auto left_3 = readers[0].read();
  const auto right_3 = readers[1].read();
  REQUIRE(left_3.has_value());
  REQUIRE(right_3.has_value());
  REQUIRE(left_3.value().value == std::optional<int>{3});
  REQUIRE(right_3.value().value == std::optional<int>{3});

  const auto left_eof = readers[0].read();
  const auto right_eof = readers[1].read();
  REQUIRE(left_eof.has_value());
  REQUIRE(right_eof.has_value());
  REQUIRE(left_eof.value().eof);
  REQUIRE(right_eof.value().eof);
}

TEST_CASE("cursor reader async public facade supports stop and delayed producer wakeup",
          "[core][cursor_reader][functional][concurrency][boundary]") {
  using scheduler_env_t = wh::testing::helper::scheduler_env<stdexec::inline_scheduler,
                                                             wh::testing::helper::stop_token>;

  {
    auto [writer, source] = wh::schema::stream::make_pipe_stream<int>(1U);
    auto readers = wh::core::make_cursor_readers(std::move(source), 1U);
    REQUIRE(readers.size() == 1U);

    using result_t = decltype(readers.front().read());
    wh::testing::helper::sender_capture<result_t> stopped_capture{};
    wh::testing::helper::stop_source stop_source{};
    auto stopped_operation = stdexec::connect(
        readers.front().read_async(),
        wh::testing::helper::sender_capture_receiver<result_t, scheduler_env_t>{
            &stopped_capture,
            {.scheduler = stdexec::inline_scheduler{}, .stop_token = stop_source.get_token()}});

    stdexec::start(stopped_operation);
    REQUIRE_FALSE(stopped_capture.ready.try_acquire());
    stop_source.request_stop();
    REQUIRE(stopped_capture.ready.try_acquire());
    REQUIRE(stopped_capture.terminal == wh::testing::helper::sender_terminal_kind::stopped);
    REQUIRE(writer.close().has_value());
  }

  {
    auto [writer, source] = wh::schema::stream::make_pipe_stream<int>(1U);
    auto readers = wh::core::make_cursor_readers(std::move(source), 1U);
    REQUIRE(readers.size() == 1U);

    wh::testing::helper::static_thread_scheduler_helper scheduler{1U};
    wh::testing::helper::joining_thread producer([stream_writer = std::move(writer)]() mutable {
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
      REQUIRE(stream_writer.try_write(17).has_value());
      REQUIRE(stream_writer.close().has_value());
    });

    auto waited =
        stdexec::sync_wait(stdexec::starts_on(scheduler.scheduler(), readers.front().read_async()));
    REQUIRE(waited.has_value());
    auto value = std::move(std::get<0>(waited.value()));
    REQUIRE(value.has_value());
    REQUIRE(value.value().value.has_value());
    REQUIRE(*value.value().value == 17);
  }
}
