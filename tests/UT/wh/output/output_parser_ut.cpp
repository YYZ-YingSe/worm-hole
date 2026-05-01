#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "wh/output/output_parser.hpp"
#include "wh/schema/stream/algorithm/collect_stream.hpp"
#include "wh/schema/stream/reader/values_stream_reader.hpp"

namespace {

struct int_parser : wh::output::output_parser_base<int_parser, std::string, int> {
  std::vector<std::string> errors{};

  [[nodiscard]] auto parse_value_impl(const std::string &input,
                                      const wh::callbacks::event_payload &)
      -> wh::core::result<int> {
    if (input == "bad") {
      return wh::core::result<int>::failure(wh::core::errc::parse_error);
    }
    return std::stoi(input);
  }

  [[nodiscard]] auto parse_value_view_impl(const std::string &input,
                                           const wh::callbacks::event_view &)
      -> wh::core::result<int> {
    if (input == "view") {
      return 11;
    }
    return parse_value_impl(input, {});
  }

  auto on_error_impl(const wh::core::error_code error, const wh::callbacks::event_payload &)
      -> void {
    errors.push_back(error.message());
  }
};

struct fallback_only_parser
    : wh::output::output_parser_base<fallback_only_parser, std::string, int> {
  std::vector<std::string> errors{};

  [[nodiscard]] auto parse_value_impl(const std::string &input,
                                      const wh::callbacks::event_payload &)
      -> wh::core::result<int> {
    if (input == "bad") {
      return wh::core::result<int>::failure(wh::core::errc::parse_error);
    }
    return std::stoi(input);
  }

  auto on_error_impl(const wh::core::error_code error, const wh::callbacks::event_payload &)
      -> void {
    errors.push_back(error.message());
  }
};

} // namespace

TEST_CASE("output parser parses values views and stream chunks",
          "[UT][wh/output/output_parser.hpp][output_parser_base::parse_value][branch][boundary]") {
  int_parser parser{};
  auto value = parser.parse_value(std::string{"7"});
  REQUIRE(value.has_value());
  REQUIRE(value.value() == 7);

  auto view = parser.parse_value_view(std::string{"view"});
  REQUIRE(view.has_value());
  REQUIRE(view.value() == 11);

  wh::output::output_parser_base<int_parser, std::string, int>::input_chunk chunk =
      wh::schema::stream::stream_chunk<std::string>::make_value("9");
  auto parsed_chunk = parser.parse_stream_chunk(chunk);
  REQUIRE(parsed_chunk.has_value());
  REQUIRE(parsed_chunk.value().value.has_value());
  REQUIRE(*parsed_chunk.value().value == 9);

  auto eof = parser.parse_stream_chunk(wh::schema::stream::stream_chunk<std::string>::make_eof());
  REQUIRE(eof.has_value());
  REQUIRE(eof.value().eof);
}

TEST_CASE("output parser reports parse failures and view fallback",
          "[UT][wh/output/output_parser.hpp][output_parser_base::parse_stream][branch]") {
  int_parser parser{};
  auto failed = parser.parse_value(std::string{"bad"});
  REQUIRE(failed.has_error());

  auto bad_chunk =
      parser.parse_stream_chunk(wh::schema::stream::stream_chunk<std::string>::make_value("bad"));
  REQUIRE(bad_chunk.has_error());

  auto reader = wh::schema::stream::make_values_stream_reader(std::vector<std::string>{"1", "2"});
  auto transformed = parser.parse_stream(std::move(reader));
  auto collected = wh::schema::stream::collect_stream_reader(std::move(transformed));
  REQUIRE(collected.has_value());
  REQUIRE(collected.value() == std::vector<int>{1, 2});
}

TEST_CASE("output parser falls back from view parsing and reports chunk-level errors",
          "[UT][wh/output/"
          "output_parser.hpp][output_parser_base::parse_value_view][condition][branch][boundary]") {
  fallback_only_parser parser{};

  auto value = parser.parse_value_view(std::string{"15"});
  REQUIRE(value.has_value());
  REQUIRE(value.value() == 15);

  auto failed_chunk =
      parser.parse_stream_chunk(wh::schema::stream::stream_chunk<std::string>::make_value("bad"));
  REQUIRE(failed_chunk.has_error());
  REQUIRE(failed_chunk.error() == wh::core::errc::parse_error);
  REQUIRE_FALSE(parser.errors.empty());

  auto error_chunk = wh::schema::stream::stream_chunk<std::string>::make_eof();
  error_chunk.eof = false;
  error_chunk.error = wh::core::make_error(wh::core::errc::timeout);
  auto propagated = parser.parse_stream_chunk(error_chunk);
  REQUIRE(propagated.has_error());
  REQUIRE(propagated.error() == wh::core::errc::timeout);
}
