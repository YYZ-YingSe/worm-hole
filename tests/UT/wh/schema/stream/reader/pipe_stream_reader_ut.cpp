#include <chrono>
#include <optional>
#include <variant>

#include <catch2/catch_test_macros.hpp>

#include "helper/manual_scheduler.hpp"
#include "helper/non_nothrow_value.hpp"
#include "helper/sender_capture.hpp"
#include "helper/test_thread_wait.hpp"
#include "wh/schema/stream/core/concepts.hpp"
#include "wh/schema/stream/pipe.hpp"
#include "wh/schema/stream/reader/pipe_stream_reader.hpp"

namespace {

using int_chunk_t = wh::schema::stream::stream_chunk<int>;
using int_chunk_result_t = wh::schema::stream::stream_result<int_chunk_t>;
using scheduler_t = wh::testing::helper::manual_scheduler<wh::core::detail::would_block>;
using env_t = wh::testing::helper::scheduler_env<scheduler_t, wh::testing::helper::stop_token>;
using non_nothrow_value_t = wh::testing::helper::non_nothrow_value;

static_assert(wh::schema::stream::detail::async_stream_reader<
              wh::schema::stream::pipe_stream_reader<non_nothrow_value_t>>);

auto require_value_chunk(const int_chunk_result_t &status, const int expected) -> void {
  REQUIRE(status.has_value());
  REQUIRE(status.value().value == std::optional<int>{expected});
}

auto require_eof_chunk(const int_chunk_result_t &status) -> void {
  REQUIRE(status.has_value());
  REQUIRE(status.value().is_terminal_eof());
}

} // namespace

TEST_CASE("pipe stream reader missing state surfaces not_found across sync and "
          "async entry points",
          "[UT][wh/schema/stream/reader/"
          "pipe_stream_reader.hpp][pipe_stream_reader::read_async][condition]["
          "branch][boundary]") {
  wh::schema::stream::pipe_stream_reader<int> missing{};
  auto missing_read = missing.read();
  REQUIRE(missing_read.has_error());
  REQUIRE(missing_read.error() == wh::core::errc::not_found);
  auto missing_try = missing.try_read();
  REQUIRE(std::holds_alternative<
          wh::schema::stream::stream_result<wh::schema::stream::stream_chunk<int>>>(missing_try));
  const auto &missing_try_result =
      std::get<wh::schema::stream::stream_result<wh::schema::stream::stream_chunk<int>>>(
          missing_try);
  REQUIRE(missing_try_result.has_error());
  REQUIRE(missing_try_result.error() == wh::core::errc::not_found);
  REQUIRE(missing.close().has_error());
  REQUIRE(missing.is_closed());
  REQUIRE(missing.is_source_closed());

  auto missing_async = wh::testing::helper::wait_value_on_test_thread(missing.read_async());
  REQUIRE(missing_async.has_error());
  REQUIRE(missing_async.error() == wh::core::errc::not_found);
}

TEST_CASE("pipe stream reader sync paths cover pending values buffered close and eof",
          "[UT][wh/schema/stream/reader/"
          "pipe_stream_reader.hpp][pipe_stream_reader::try_read_impl][condition]["
          "branch][boundary]") {
  auto [writer, reader] = wh::schema::stream::make_pipe_stream<int>(1U);
  auto pending = reader.try_read();
  REQUIRE(std::holds_alternative<wh::schema::stream::stream_signal>(pending));
  REQUIRE(std::get<wh::schema::stream::stream_signal>(pending) ==
          wh::schema::stream::stream_pending);

  REQUIRE(writer.try_write(5).has_value());
  auto first = std::get<wh::schema::stream::stream_result<wh::schema::stream::stream_chunk<int>>>(
      reader.try_read());
  require_value_chunk(first, 5);

  REQUIRE(writer.try_write(6).has_value());
  REQUIRE(writer.close().has_value());
  auto drained = reader.read();
  require_value_chunk(drained, 6);
  auto eof = reader.read();
  require_eof_chunk(eof);
}

TEST_CASE("pipe stream reader read_async stays available for movable values "
          "without nothrow move",
          "[UT][wh/schema/stream/reader/"
          "pipe_stream_reader.hpp][pipe_stream_reader::read_async][condition]["
          "boundary]") {
  auto [writer, reader] = wh::schema::stream::make_pipe_stream<non_nothrow_value_t>(1U);

  REQUIRE(writer.try_write(non_nothrow_value_t{7}).has_value());
  auto next = wh::testing::helper::wait_value_on_test_thread(reader.read_async());
  REQUIRE(next.has_value());
  REQUIRE(next.value().value == std::optional<non_nothrow_value_t>{non_nothrow_value_t{7}});
}

TEST_CASE("pipe stream reader read_async snapshots shared state before "
          "normalization builds the child sender",
          "[UT][wh/schema/stream/reader/"
          "pipe_stream_reader.hpp][pipe_stream_reader::read_async][regression]["
          "lifetime]") {
  using state_t = wh::schema::stream::detail::pipe_stream_state<int>;

  auto state = std::make_shared<state_t>(1U);
  wh::schema::stream::pipe_stream_reader<int> reader{state};
  REQUIRE(state->queue.try_push(17) == wh::core::bounded_queue_status::success);

  auto next = wh::testing::helper::wait_value_on_test_thread(reader.read_async());
  require_value_chunk(next, 17);
}

TEST_CASE("pipe stream reader close path returns eof on subsequent reads and "
          "tolerates auto-close toggles",
          "[UT][wh/schema/stream/reader/"
          "pipe_stream_reader.hpp][pipe_stream_reader::close_impl][condition]["
          "branch]") {
  auto [writer, reader] = wh::schema::stream::make_pipe_stream<int>(2U);
  reader.set_automatic_close(wh::schema::stream::auto_close_disabled);
  reader.set_automatic_close(wh::schema::stream::auto_close_enabled);

  REQUIRE(writer.try_write(9).has_value());
  REQUIRE(reader.close().has_value());
  REQUIRE(reader.close().has_value());
  REQUIRE(reader.is_closed());
  REQUIRE(reader.is_source_closed());

  auto try_result = reader.try_read();
  REQUIRE(std::holds_alternative<int_chunk_result_t>(try_result));
  require_eof_chunk(std::get<int_chunk_result_t>(try_result));

  auto sync_result = reader.read();
  require_eof_chunk(sync_result);

  auto async_result = wh::testing::helper::wait_value_on_test_thread(reader.read_async());
  require_eof_chunk(async_result);
}

TEST_CASE("pipe stream reader read_async covers immediate value and controlled "
          "interleaving wakeups",
          "[UT][wh/schema/stream/reader/"
          "pipe_stream_reader.hpp][pipe_stream_reader::read_async][condition]["
          "branch][concurrency]") {
  using namespace std::chrono_literals;

  auto [immediate_writer, immediate_reader] = wh::schema::stream::make_pipe_stream<int>(2U);
  REQUIRE(immediate_writer.try_write(9).has_value());
  auto immediate = wh::testing::helper::wait_value_on_test_thread(immediate_reader.read_async());
  require_value_chunk(immediate, 9);

  wh::testing::helper::manual_scheduler_state scheduler_state{};
  scheduler_t scheduler{&scheduler_state};
  env_t env{scheduler, {}};

  for (int iteration = 0; iteration != 16; ++iteration) {
    auto [writer, reader] = wh::schema::stream::make_pipe_stream<int>(1U);
    wh::testing::helper::sender_capture<int_chunk_result_t> capture{};
    auto operation = stdexec::connect(
        reader.read_async(),
        wh::testing::helper::sender_capture_receiver<int_chunk_result_t, env_t>{&capture, env});
    stdexec::start(operation);

    REQUIRE_FALSE(capture.ready.try_acquire());
    REQUIRE(writer.try_write(iteration).has_value());
    if (!capture.ready.try_acquire()) {
      REQUIRE(scheduler_state.run_one());
      REQUIRE(capture.ready.try_acquire_for(500ms));
    }
    REQUIRE(capture.terminal == wh::testing::helper::sender_terminal_kind::value);
    REQUIRE(capture.value.has_value());
    require_value_chunk(*capture.value, iteration);
  }

  for (int iteration = 0; iteration != 16; ++iteration) {
    auto [writer, reader] = wh::schema::stream::make_pipe_stream<int>(1U);
    wh::testing::helper::sender_capture<int_chunk_result_t> capture{};
    auto operation = stdexec::connect(
        reader.read_async(),
        wh::testing::helper::sender_capture_receiver<int_chunk_result_t, env_t>{&capture, env});
    stdexec::start(operation);

    REQUIRE_FALSE(capture.ready.try_acquire());
    REQUIRE(writer.close().has_value());
    if (!capture.ready.try_acquire()) {
      REQUIRE(scheduler_state.run_one());
      REQUIRE(capture.ready.try_acquire_for(500ms));
    }
    REQUIRE(capture.terminal == wh::testing::helper::sender_terminal_kind::value);
    REQUIRE(capture.value.has_value());
    require_eof_chunk(*capture.value);
  }
}
