#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <string>

#include "helper/manual_scheduler.hpp"
#include "helper/non_nothrow_value.hpp"
#include "helper/sender_capture.hpp"
#include "helper/test_thread_wait.hpp"
#include "wh/schema/stream/pipe.hpp"
#include "wh/schema/stream/writer/pipe_stream_writer.hpp"

namespace {

using write_result_t = wh::core::result<void>;
using scheduler_t =
    wh::testing::helper::manual_scheduler<wh::core::detail::would_block>;
using env_t = wh::testing::helper::scheduler_env<
    scheduler_t, wh::testing::helper::stop_token>;
using non_nothrow_value_t = wh::testing::helper::non_nothrow_value;

static_assert(requires(
    wh::schema::stream::pipe_stream_writer<non_nothrow_value_t> &writer,
    const non_nothrow_value_t &copy_value, non_nothrow_value_t move_value) {
  writer.try_write(copy_value);
  writer.try_write(std::move(move_value));
  writer.write_async(copy_value);
  writer.write_async(std::move(move_value));
});

} // namespace

TEST_CASE("pipe stream writer missing state surfaces not_found across sync and "
          "async entry points",
          "[UT][wh/schema/stream/writer/"
          "pipe_stream_writer.hpp][pipe_stream_writer::write_async][condition]["
          "branch][boundary]") {
  wh::schema::stream::pipe_stream_writer<int> missing{};
  auto missing_write = missing.try_write(1);
  REQUIRE(missing_write.has_error());
  REQUIRE(missing_write.error() == wh::core::errc::not_found);
  REQUIRE(missing.close().has_error());
  REQUIRE(missing.is_closed());

  auto missing_async =
      wh::testing::helper::wait_value_on_test_thread(missing.write_async(7));
  REQUIRE(missing_async.has_error());
  REQUIRE(missing_async.error() == wh::core::errc::not_found);
}

TEST_CASE("pipe stream writer sync paths cover lvalue rvalue full and closed "
          "branches",
          "[UT][wh/schema/stream/writer/"
          "pipe_stream_writer.hpp][pipe_stream_writer::try_write][condition]["
          "branch][boundary]") {
  auto [writer, reader] = wh::schema::stream::make_pipe_stream<int>(1U);
  const int alpha = 1;
  REQUIRE(writer.try_write(alpha).has_value());
  auto alpha_chunk = reader.read();
  REQUIRE(alpha_chunk.has_value());
  REQUIRE(alpha_chunk.value().value == std::optional<int>{1});
  REQUIRE(alpha == 1);

  REQUIRE(writer.try_write(2).has_value());
  auto beta_chunk = reader.read();
  REQUIRE(beta_chunk.has_value());
  REQUIRE(beta_chunk.value().value == std::optional<int>{2});

  REQUIRE(writer.try_write(3).has_value());
  auto full = writer.try_write(4);
  REQUIRE(full.has_error());
  REQUIRE(full.error() == wh::core::errc::queue_full);

  REQUIRE(reader.close().has_value());
  auto closed = writer.try_write(5);
  REQUIRE(closed.has_error());
  REQUIRE(closed.error() == wh::core::errc::channel_closed);
}

TEST_CASE(
    "pipe stream writer close and async fast paths return stable completion "
    "states",
    "[UT][wh/schema/stream/writer/"
    "pipe_stream_writer.hpp][pipe_stream_writer::close][condition][branch]") {
  auto [writer, reader] = wh::schema::stream::make_pipe_stream<std::string>(2U);
  auto async_status = wh::testing::helper::wait_value_on_test_thread(
      writer.write_async(std::string{"delta"}));
  REQUIRE(async_status.has_value());
  auto async_chunk = reader.read();
  REQUIRE(async_chunk.has_value());
  REQUIRE(async_chunk.value().value == std::optional<std::string>{"delta"});

  REQUIRE(writer.close().has_value());
  REQUIRE(writer.close().has_value());
  REQUIRE(writer.is_closed());
  auto closed_write = writer.try_write(std::string{"epsilon"});
  REQUIRE(closed_write.has_error());
  REQUIRE(closed_write.error() == wh::core::errc::channel_closed);

  auto [closed_writer, closed_reader] =
      wh::schema::stream::make_pipe_stream<std::string>(1U);
  REQUIRE(closed_reader.close().has_value());
  auto closed_async = wh::testing::helper::wait_value_on_test_thread(
      closed_writer.write_async(std::string{"zeta"}));
  REQUIRE(closed_async.has_error());
  REQUIRE(closed_async.error() == wh::core::errc::channel_closed);
}

TEST_CASE("pipe stream writer accepts copyable and movable values without "
          "nothrow guarantees",
          "[UT][wh/schema/stream/writer/"
          "pipe_stream_writer.hpp][pipe_stream_writer::try_write][condition]["
          "boundary]") {
  auto [writer, reader] =
      wh::schema::stream::make_pipe_stream<non_nothrow_value_t>(2U);

  const non_nothrow_value_t copied{11};
  REQUIRE(writer.try_write(copied).has_value());
  auto copied_chunk = reader.read();
  REQUIRE(copied_chunk.has_value());
  REQUIRE(copied_chunk.value().value ==
          std::optional<non_nothrow_value_t>{non_nothrow_value_t{11}});

  auto async_status = wh::testing::helper::wait_value_on_test_thread(
      writer.write_async(non_nothrow_value_t{12}));
  REQUIRE(async_status.has_value());
  auto moved_chunk = reader.read();
  REQUIRE(moved_chunk.has_value());
  REQUIRE(moved_chunk.value().value ==
          std::optional<non_nothrow_value_t>{non_nothrow_value_t{12}});
}

TEST_CASE("pipe stream writer write_async snapshots shared state before "
          "normalization builds the child sender",
          "[UT][wh/schema/stream/writer/"
          "pipe_stream_writer.hpp][pipe_stream_writer::write_async][regression]["
          "lifetime]") {
  using state_t = wh::schema::stream::detail::pipe_stream_state<int>;

  auto state = std::make_shared<state_t>(1U);
  wh::schema::stream::pipe_stream_writer<int> writer{state};

  auto status =
      wh::testing::helper::wait_value_on_test_thread(writer.write_async(23));
  REQUIRE(status.has_value());

  auto drained = state->queue.pop();
  REQUIRE(drained == std::optional<int>{23});
}

TEST_CASE("pipe stream writer write_async covers controlled interleaving with "
          "pop and close",
          "[UT][wh/schema/stream/writer/"
          "pipe_stream_writer.hpp][pipe_stream_writer::write_async][branch]["
          "concurrency]") {
  using namespace std::chrono_literals;

  wh::testing::helper::manual_scheduler_state scheduler_state{};
  scheduler_t scheduler{&scheduler_state};
  env_t env{scheduler, {}};

  for (int iteration = 0; iteration != 16; ++iteration) {
    auto [writer, reader] = wh::schema::stream::make_pipe_stream<int>(1U);
    REQUIRE(writer.try_write(iteration).has_value());

    wh::testing::helper::sender_capture<write_result_t> capture{};
    auto operation = stdexec::connect(
        writer.write_async(iteration + 1),
        wh::testing::helper::sender_capture_receiver<write_result_t, env_t>{
            &capture, env});
    stdexec::start(operation);

    REQUIRE_FALSE(capture.ready.try_acquire());
    auto drained = reader.read();
    REQUIRE(drained.has_value());
    REQUIRE(drained.value().value == std::optional<int>{iteration});
    if (!capture.ready.try_acquire()) {
      REQUIRE(scheduler_state.run_one());
      REQUIRE(capture.ready.try_acquire_for(500ms));
    }
    REQUIRE(capture.terminal ==
            wh::testing::helper::sender_terminal_kind::value);
    REQUIRE(capture.value.has_value());
    REQUIRE(capture.value->has_value());

    auto written = reader.read();
    REQUIRE(written.has_value());
    REQUIRE(written.value().value == std::optional<int>{iteration + 1});
  }

  for (int iteration = 0; iteration != 16; ++iteration) {
    auto [writer, reader] = wh::schema::stream::make_pipe_stream<int>(1U);
    REQUIRE(writer.try_write(iteration).has_value());

    wh::testing::helper::sender_capture<write_result_t> capture{};
    auto operation = stdexec::connect(
        writer.write_async(iteration + 1),
        wh::testing::helper::sender_capture_receiver<write_result_t, env_t>{
            &capture, env});
    stdexec::start(operation);

    REQUIRE_FALSE(capture.ready.try_acquire());
    REQUIRE(reader.close().has_value());
    if (!capture.ready.try_acquire()) {
      REQUIRE(scheduler_state.run_one());
      REQUIRE(capture.ready.try_acquire_for(500ms));
    }
    REQUIRE(capture.terminal ==
            wh::testing::helper::sender_terminal_kind::value);
    REQUIRE(capture.value.has_value());
    REQUIRE(capture.value->has_error());
    REQUIRE(capture.value->error() == wh::core::errc::channel_closed);
  }
}
