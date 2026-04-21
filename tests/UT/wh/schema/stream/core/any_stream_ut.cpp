#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "helper/non_nothrow_value.hpp"
#include "helper/sender_capture.hpp"
#include "helper/sender_env.hpp"
#include "helper/test_thread_wait.hpp"
#include "wh/schema/stream/core/any_stream.hpp"
#include "wh/schema/stream/core/concepts.hpp"
#include "wh/schema/stream/pipe.hpp"

namespace {

struct auto_close_probe_reader : wh::schema::stream::stream_base<auto_close_probe_reader, int> {
  bool automatic_close{true};

  [[nodiscard]] auto read_impl()
      -> wh::schema::stream::stream_result<wh::schema::stream::stream_chunk<int>> {
    return wh::schema::stream::stream_chunk<int>::make_eof();
  }

  [[nodiscard]] auto try_read_impl()
      -> wh::schema::stream::stream_try_result<wh::schema::stream::stream_chunk<int>> {
    return wh::schema::stream::stream_result<wh::schema::stream::stream_chunk<int>>{
        wh::schema::stream::stream_chunk<int>::make_eof()};
  }

  [[nodiscard]] auto read_async() {
    using result_t = wh::schema::stream::stream_result<wh::schema::stream::stream_chunk<int>>;
    return stdexec::just(result_t{wh::schema::stream::stream_chunk<int>::make_eof()});
  }

  auto close_impl() -> wh::core::result<void> { return {}; }
  [[nodiscard]] auto is_closed_impl() const noexcept -> bool { return false; }
  [[nodiscard]] auto is_source_closed() const noexcept -> bool { return false; }
  auto set_automatic_close(const wh::schema::stream::auto_close_options &options) -> void {
    automatic_close = options.enabled;
  }
};

using non_nothrow_value_t = wh::testing::helper::non_nothrow_value;

static_assert(wh::schema::stream::detail::async_stream_reader<
              wh::schema::stream::pipe_stream_reader<non_nothrow_value_t>>);
static_assert(requires {
  wh::schema::stream::any_stream_reader<non_nothrow_value_t>{
      std::declval<wh::schema::stream::pipe_stream_reader<non_nothrow_value_t> &&>()};
  wh::schema::stream::any_stream_writer<non_nothrow_value_t>{
      std::declval<wh::schema::stream::pipe_stream_writer<non_nothrow_value_t> &&>()};
});

} // namespace

TEST_CASE("any stream reader covers empty target move target_if and auto-close "
          "forwarding",
          "[UT][wh/schema/stream/core/"
          "any_stream.hpp][any_stream_reader::read_async][branch][boundary]") {
  using result_t = wh::schema::stream::stream_result<wh::schema::stream::stream_chunk<int>>;

  wh::schema::stream::any_stream_reader<int> empty{};
  auto missing_read = empty.read();
  REQUIRE(missing_read.has_error());
  REQUIRE(missing_read.error() == wh::core::errc::not_found);
  auto missing_try = empty.try_read();
  REQUIRE(std::holds_alternative<result_t>(missing_try));
  REQUIRE(std::get<result_t>(missing_try).has_error());
  REQUIRE(empty.close().has_error());
  REQUIRE(empty.is_closed());
  REQUIRE(empty.is_source_closed());
  auto missing_async = wh::testing::helper::wait_value_on_test_thread(empty.read_async());
  REQUIRE(missing_async.has_error());
  REQUIRE(missing_async.error() == wh::core::errc::not_found);

  wh::schema::stream::any_stream_reader<int> erased{auto_close_probe_reader{}};
  auto *typed = erased.target_if<auto_close_probe_reader>();
  REQUIRE(typed != nullptr);
  REQUIRE(typed->automatic_close);
  erased.set_automatic_close(wh::schema::stream::auto_close_disabled);
  REQUIRE_FALSE(typed->automatic_close);

  auto moved = std::move(erased);
  REQUIRE(erased.is_closed());
  REQUIRE(moved.target_if<auto_close_probe_reader>() != nullptr);
}

TEST_CASE("any stream read and write async paths preserve stop and deliver "
          "through erased backend",
          "[UT][wh/schema/stream/core/"
          "any_stream.hpp][any_stream_writer::write_async][condition]["
          "concurrency][branch]") {
  auto [writer, reader] = wh::schema::stream::make_pipe_stream<int>(1U);
  wh::schema::stream::any_stream_reader<int> erased_reader{std::move(reader)};

  stdexec::inplace_stop_source read_stop{};
  using read_result_t = wh::schema::stream::stream_result<wh::schema::stream::stream_chunk<int>>;
  using read_env_t =
      wh::testing::helper::scheduler_env<stdexec::inline_scheduler, stdexec::inplace_stop_token>;
  wh::testing::helper::sender_capture<read_result_t> read_capture{};
  auto read_op = stdexec::connect(
      erased_reader.read_async(),
      wh::testing::helper::sender_capture_receiver<read_result_t, read_env_t>{
          &read_capture,
          {.scheduler = stdexec::inline_scheduler{}, .stop_token = read_stop.get_token()}});
  stdexec::start(read_op);
  REQUIRE_FALSE(read_capture.ready.try_acquire());
  read_stop.request_stop();
  REQUIRE(read_capture.ready.try_acquire());
  REQUIRE(read_capture.terminal == wh::testing::helper::sender_terminal_kind::stopped);

  REQUIRE(writer.try_write(42).has_value());
  REQUIRE(writer.close().has_value());
  auto recovered = wh::testing::helper::wait_value_on_test_thread(erased_reader.read_async());
  REQUIRE(recovered.has_value());
  REQUIRE(recovered.value().value == std::optional<int>{42});

  auto [raw_writer, raw_reader] = wh::schema::stream::make_pipe_stream<int>(1U);
  REQUIRE(raw_writer.try_write(1).has_value());
  wh::schema::stream::any_stream_writer<int> erased_writer{std::move(raw_writer)};
  stdexec::inplace_stop_source write_stop{};
  using write_env_t =
      wh::testing::helper::scheduler_env<stdexec::inline_scheduler, stdexec::inplace_stop_token>;
  wh::testing::helper::sender_capture<wh::core::result<void>> write_capture{};
  auto write_op = stdexec::connect(
      erased_writer.write_async(2),
      wh::testing::helper::sender_capture_receiver<wh::core::result<void>, write_env_t>{
          &write_capture,
          {.scheduler = stdexec::inline_scheduler{}, .stop_token = write_stop.get_token()}});
  stdexec::start(write_op);
  REQUIRE_FALSE(write_capture.ready.try_acquire());
  write_stop.request_stop();
  REQUIRE(write_capture.ready.try_acquire());
  REQUIRE(write_capture.terminal == wh::testing::helper::sender_terminal_kind::stopped);
  REQUIRE(raw_reader.close().has_value());

  wh::schema::stream::any_stream_writer<int> default_writer{};
  REQUIRE(default_writer.try_write(7).has_error());
  auto missing_write =
      wh::testing::helper::wait_value_on_test_thread(default_writer.write_async(7));
  REQUIRE(missing_write.has_error());
  REQUIRE(missing_write.error() == wh::core::errc::not_found);
}

TEST_CASE("any stream erases pipe backends for values without nothrow copy or move",
          "[UT][wh/schema/stream/core/"
          "any_stream.hpp][any_stream_reader::read_async][condition][boundary]") {
  auto [raw_writer, raw_reader] = wh::schema::stream::make_pipe_stream<non_nothrow_value_t>(2U);
  wh::schema::stream::any_stream_writer<non_nothrow_value_t> erased_writer{std::move(raw_writer)};
  wh::schema::stream::any_stream_reader<non_nothrow_value_t> erased_reader{std::move(raw_reader)};

  REQUIRE(erased_writer.try_write(non_nothrow_value_t{21}).has_value());
  auto first = wh::testing::helper::wait_value_on_test_thread(erased_reader.read_async());
  REQUIRE(first.has_value());
  REQUIRE(first.value().value == std::optional<non_nothrow_value_t>{non_nothrow_value_t{21}});

  auto async_status = wh::testing::helper::wait_value_on_test_thread(
      erased_writer.write_async(non_nothrow_value_t{22}));
  REQUIRE(async_status.has_value());
  auto second = erased_reader.read();
  REQUIRE(second.has_value());
  REQUIRE(second.value().value == std::optional<non_nothrow_value_t>{non_nothrow_value_t{22}});
}
