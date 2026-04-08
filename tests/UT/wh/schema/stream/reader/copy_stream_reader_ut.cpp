#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include "wh/schema/stream/pipe.hpp"
#include "wh/schema/stream/reader/copy_stream_reader.hpp"
#include "wh/schema/stream/reader/values_stream_reader.hpp"

TEST_CASE("copy stream readers keep independent cursors and support single-copy boundary",
          "[UT][wh/schema/stream/reader/copy_stream_reader.hpp][make_copy_stream_readers][branch][boundary]") {
  auto [writer, reader] = wh::schema::stream::make_pipe_stream<std::string>(4U);
  REQUIRE(writer.try_write("alpha").has_value());
  REQUIRE(writer.try_write("beta").has_value());
  REQUIRE(writer.close().has_value());

  auto copies = wh::schema::stream::make_copy_stream_readers(std::move(reader), 2U);
  REQUIRE(copies.size() == 2U);

  auto drain = [](auto &copy_reader) {
    std::vector<std::string> values{};
    for (int i = 0; i < 16; ++i) {
      auto chunk = copy_reader.try_read();
      if (std::holds_alternative<wh::schema::stream::stream_signal>(chunk)) {
        continue;
      }
      auto next = std::move(std::get<wh::schema::stream::stream_result<
          wh::schema::stream::stream_chunk<std::string>>>(chunk));
      if (next.has_error()) {
        FAIL("unexpected error while draining copy reader");
      }
      if (next.value().eof) {
        break;
      }
      values.push_back(*next.value().value);
    }
    return values;
  };

  REQUIRE(drain(copies[0]) == std::vector<std::string>({"alpha", "beta"}));
  REQUIRE(drain(copies[1]) == std::vector<std::string>({"alpha", "beta"}));

  auto [single_writer, single_reader] = wh::schema::stream::make_pipe_stream<int>(2U);
  REQUIRE(single_writer.try_write(1).has_value());
  REQUIRE(single_writer.close().has_value());
  auto single = wh::schema::stream::make_copy_stream_readers(std::move(single_reader), 1U);
  REQUIRE(single.size() == 1U);
  auto first = std::get<wh::schema::stream::stream_result<
      wh::schema::stream::stream_chunk<int>>>(single.front().try_read());
  REQUIRE(first.has_value());
  REQUIRE(first.value().value == std::optional<int>{1});
}

TEST_CASE("copy stream readers async waits resume independently after upstream write",
          "[UT][wh/schema/stream/reader/copy_stream_reader.hpp][copy_stream_reader::read_async][concurrency][branch]") {
  auto [writer, reader] = wh::schema::stream::make_pipe_stream<int>(4U);
  auto copies = wh::schema::stream::make_copy_stream_readers(std::move(reader), 2U);
  REQUIRE(copies.size() == 2U);

  exec::static_thread_pool pool{1U};
  std::optional<wh::core::result<void>> write_status{};
  std::optional<wh::core::result<void>> close_status{};

  std::jthread producer([stream_writer = std::move(writer), &write_status,
                         &close_status]() mutable {
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    write_status.emplace(stream_writer.try_write(11));
    close_status.emplace(stream_writer.close());
  });

  auto waited = stdexec::sync_wait(stdexec::when_all(
      stdexec::starts_on(pool.get_scheduler(), copies[0].read_async()),
      stdexec::starts_on(pool.get_scheduler(), copies[1].read_async())));

  REQUIRE(waited.has_value());
  REQUIRE(write_status.has_value());
  REQUIRE(write_status->has_value());
  REQUIRE(close_status.has_value());
  REQUIRE(close_status->has_value());

  auto first = std::move(std::get<0>(waited.value()));
  auto second = std::move(std::get<1>(waited.value()));
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  REQUIRE(first.value().value == std::optional<int>{11});
  REQUIRE(second.value().value == std::optional<int>{11});
}

TEST_CASE("copy stream readers preserve source-closed query and auto-close forwarding on copied values source",
          "[UT][wh/schema/stream/reader/copy_stream_reader.hpp][copy_stream_reader::set_automatic_close][condition]") {
  auto copies = wh::schema::stream::make_copy_stream_readers(
      wh::schema::stream::make_values_stream_reader(std::vector<std::string>{
          "left", "right"}),
      2U);
  REQUIRE(copies.size() == 2U);
  copies[0].set_automatic_close(wh::schema::stream::auto_close_disabled);
  copies[1].set_automatic_close(wh::schema::stream::auto_close_disabled);

  REQUIRE_FALSE(copies[0].is_source_closed());
  REQUIRE_FALSE(copies[1].is_source_closed());

  (void)copies[0].read();
  (void)copies[0].read();
  (void)copies[0].read();
  (void)copies[1].read();
  (void)copies[1].read();
  (void)copies[1].read();

  REQUIRE(copies[0].is_source_closed());
  REQUIRE(copies[1].is_source_closed());
}
