#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "wh/schema/stream.hpp"

TEST_CASE("stream facade composes pipe reader writer and algorithms",
          "[UT][wh/schema/stream.hpp][make_pipe_stream][branch]") {
  auto [writer, reader] = wh::schema::stream::make_pipe_stream<std::string>(4U);
  REQUIRE(writer.try_write(std::string{"hello"}).has_value());
  REQUIRE(writer.close().has_value());

  auto collected = wh::schema::stream::collect_text_stream_reader(std::move(reader));
  REQUIRE(collected.has_value());
  REQUIRE(collected.value() == "hello");
}

TEST_CASE(
    "stream facade also exposes value readers and empty collection through the umbrella header",
    "[UT][wh/schema/stream.hpp][collect_stream_reader][condition][boundary]") {
  auto reader = wh::schema::stream::make_values_stream_reader(std::vector<int>{});

  auto collected = wh::schema::stream::collect_stream_reader(std::move(reader));
  REQUIRE(collected.has_value());
  REQUIRE(collected.value().empty());
}
