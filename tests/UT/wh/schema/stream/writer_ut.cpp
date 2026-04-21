#include <catch2/catch_test_macros.hpp>

#include "wh/schema/stream/writer.hpp"

TEST_CASE("stream writer facade exports pipe writer through public header",
          "[UT][wh/schema/stream/writer.hpp][pipe_stream_writer][branch][boundary]") {
  wh::schema::stream::pipe_stream_writer<int> writer{};

  auto write_status = writer.try_write(7);
  REQUIRE(write_status.has_error());
  REQUIRE(write_status.error() == wh::core::errc::not_found);
  REQUIRE(writer.is_closed());
}

TEST_CASE(
    "stream writer facade exports erased writer defaults through public header",
    "[UT][wh/schema/stream/writer.hpp][any_stream_writer::close][condition][branch][boundary]") {
  wh::schema::stream::any_stream_writer<int> writer{};

  auto write_status = writer.try_write(3);
  REQUIRE(write_status.has_error());
  REQUIRE(write_status.error() == wh::core::errc::not_found);

  auto close_status = writer.close();
  REQUIRE(close_status.has_error());
  REQUIRE(close_status.error() == wh::core::errc::not_found);
  REQUIRE(writer.is_closed());
}
