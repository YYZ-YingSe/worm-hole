#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

#include "wh/schema/stream/pipe.hpp"

TEST_CASE("pipe facade creates connected reader writer pair and normalizes zero capacity",
          "[UT][wh/schema/stream/pipe.hpp][make_pipe_stream][branch][boundary]") {
  auto [writer, reader] = wh::schema::stream::make_pipe_stream<int>(0U);

  REQUIRE(writer.try_write(1).has_value());
  auto second = writer.try_write(2);
  REQUIRE(second.has_error());
  REQUIRE(second.error() == wh::core::errc::queue_full);

  auto first = std::get<wh::schema::stream::stream_result<
      wh::schema::stream::stream_chunk<int>>>(reader.try_read());
  REQUIRE(first.has_value());
  REQUIRE(first.value().value == std::optional<int>{1});
}

TEST_CASE("pipe facade preserves buffered values across close and eof",
          "[UT][wh/schema/stream/pipe.hpp][make_pipe_stream][condition][boundary]") {
  auto [writer, reader] = wh::schema::stream::make_pipe_stream<std::string>(2U);

  REQUIRE(writer.try_write(std::string{"a"}).has_value());
  REQUIRE(writer.try_write(std::string{"b"}).has_value());
  REQUIRE(writer.close().has_value());

  auto first = reader.read();
  REQUIRE(first.has_value());
  REQUIRE(first.value().value == std::optional<std::string>{"a"});

  auto second = reader.read();
  REQUIRE(second.has_value());
  REQUIRE(second.value().value == std::optional<std::string>{"b"});

  auto eof = reader.read();
  REQUIRE(eof.has_value());
  REQUIRE(eof.value().is_terminal_eof());
}
