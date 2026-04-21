#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "wh/core/error.hpp"
#include "wh/output/output_parser.hpp"
#include "wh/schema/stream.hpp"

namespace {

struct int_parser final : wh::output::output_parser_base<int_parser, std::string, int> {
  std::vector<wh::core::error_code> errors{};

  auto parse_value_impl(const std::string &input, const wh::callbacks::event_payload &)
      -> wh::core::result<int> {
    try {
      return std::stoi(input);
    } catch (...) {
      return wh::core::result<int>::failure(wh::core::errc::parse_error);
    }
  }

  auto on_error_impl(const wh::core::error_code error, const wh::callbacks::event_payload &)
      -> void {
    errors.push_back(error);
  }
};

struct int_parser_view_preferred final
    : wh::output::output_parser_base<int_parser_view_preferred, std::string, int> {
  bool view_used{false};

  auto parse_value_impl(const std::string &, const wh::callbacks::event_payload &)
      -> wh::core::result<int> {
    return wh::core::result<int>::failure(wh::core::errc::parse_error);
  }

  auto parse_value_view_impl(const std::string &input, const wh::callbacks::event_view &)
      -> wh::core::result<int> {
    view_used = true;
    try {
      return std::stoi(input);
    } catch (...) {
      return wh::core::result<int>::failure(wh::core::errc::parse_error);
    }
  }
};

} // namespace

TEST_CASE("output parser base handles value and stream chunk paths", "[core][output][functional]") {
  int_parser parser{};

  auto value = parser.parse_value("42");
  REQUIRE(value.has_value());
  REQUIRE(value.value() == 42);

  auto bad_value = parser.parse_value("bad");
  REQUIRE(bad_value.has_error());
  REQUIRE(bad_value.error() == wh::core::errc::parse_error);

  wh::schema::stream::stream_chunk<std::string> input_chunk =
      wh::schema::stream::stream_chunk<std::string>::make_value("7");
  auto parsed_chunk = parser.parse_stream_chunk(input_chunk);
  REQUIRE(parsed_chunk.has_value());
  REQUIRE(parsed_chunk.value().value.has_value());
  REQUIRE(*parsed_chunk.value().value == 7);

  wh::schema::stream::stream_chunk<std::string> eof_chunk =
      wh::schema::stream::stream_chunk<std::string>::make_eof();
  auto parsed_eof = parser.parse_stream_chunk(eof_chunk);
  REQUIRE(parsed_eof.has_value());
  REQUIRE(parsed_eof.value().eof);

  wh::schema::stream::stream_chunk<std::string> bad_chunk =
      wh::schema::stream::stream_chunk<std::string>::make_value("x");
  auto parsed_bad = parser.parse_stream_chunk(bad_chunk);
  REQUIRE(parsed_bad.has_error());
  REQUIRE(parsed_bad.error() == wh::core::errc::parse_error);
  REQUIRE(!parser.errors.empty());
}

TEST_CASE("output parser parse_stream closes upstream on parse failure",
          "[core][output][functional]") {
  int_parser parser{};
  auto [writer, reader] = wh::schema::stream::make_pipe_stream<std::string>(4U);
  REQUIRE(writer.try_write("42").has_value());
  REQUIRE(writer.try_write("bad").has_value());

  auto parsed_reader = parser.parse_stream(std::move(reader));
  auto first = std::get<wh::schema::stream::stream_result<wh::schema::stream::stream_chunk<int>>>(
      parsed_reader.try_read());
  REQUIRE(first.has_value());
  REQUIRE(first.value().value.has_value());
  REQUIRE(*first.value().value == 42);

  auto second = std::get<wh::schema::stream::stream_result<wh::schema::stream::stream_chunk<int>>>(
      parsed_reader.try_read());
  REQUIRE(second.has_value());
  REQUIRE(second.value().error == wh::core::errc::parse_error);
  REQUIRE(!parser.errors.empty());
  REQUIRE(parser.errors.back().code() == wh::core::errc::parse_error);

  auto write_after_error = writer.try_write("99");
  REQUIRE(write_after_error.has_error());
  REQUIRE(write_after_error.error() == wh::core::errc::channel_closed);
}

TEST_CASE("output parser parse_stream_view prefers view implementation",
          "[core][output][functional]") {
  int_parser_view_preferred parser{};
  auto [writer, reader] = wh::schema::stream::make_pipe_stream<std::string>(2U);
  REQUIRE(writer.try_write("11").has_value());
  REQUIRE(writer.close().has_value());

  auto parsed_reader = parser.parse_stream_view(std::move(reader));
  auto first = std::get<wh::schema::stream::stream_result<wh::schema::stream::stream_chunk<int>>>(
      parsed_reader.try_read());
  REQUIRE(first.has_value());
  REQUIRE(first.value().value.has_value());
  REQUIRE(*first.value().value == 11);
  REQUIRE(parser.view_used);
}
