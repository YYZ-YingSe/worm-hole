#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "wh/schema/stream.hpp"

TEST_CASE("schema stream public facade builds pipe streams and collects value and text readers",
          "[core][schema][stream][functional]") {
  auto [value_writer, value_reader] = wh::schema::stream::make_pipe_stream<int>(4U);
  REQUIRE(value_writer.try_write(1).has_value());
  REQUIRE(value_writer.try_write(2).has_value());
  REQUIRE(value_writer.try_write(3).has_value());
  REQUIRE(value_writer.close().has_value());

  auto collected = wh::schema::stream::collect_stream_reader(std::move(value_reader));
  REQUIRE(collected.has_value());
  REQUIRE(collected.value() == std::vector<int>{1, 2, 3});

  auto [text_writer, text_reader] =
      wh::schema::stream::make_pipe_stream<std::string>(4U);
  REQUIRE(text_writer.try_write(std::string{"a"}).has_value());
  REQUIRE(text_writer.try_write(std::string{"b"}).has_value());
  REQUIRE(text_writer.try_write(std::string{"c"}).has_value());
  REQUIRE(text_writer.close().has_value());

  auto text =
      wh::schema::stream::collect_text_stream_reader(std::move(text_reader));
  REQUIRE(text.has_value());
  REQUIRE(text.value() == "abc");
}

TEST_CASE("schema stream public facade preserves buffered values across close and eof",
          "[core][schema][stream][functional][boundary]") {
  auto [writer, reader] = wh::schema::stream::make_pipe_stream<std::string>(2U);
  REQUIRE(writer.try_write(std::string{"alpha"}).has_value());
  REQUIRE(writer.try_write(std::string{"beta"}).has_value());
  REQUIRE(writer.close().has_value());
  REQUIRE(writer.is_closed());
  REQUIRE(writer.try_write(std::string{"gamma"}).has_error());

  auto first = reader.read();
  auto second = reader.read();
  auto eof = reader.read();
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  REQUIRE(eof.has_value());
  REQUIRE(first.value().value == std::string{"alpha"});
  REQUIRE(second.value().value == std::string{"beta"});
  REQUIRE(eof.value().is_terminal_eof());
}
