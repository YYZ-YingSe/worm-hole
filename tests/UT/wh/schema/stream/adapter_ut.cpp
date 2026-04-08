#include <catch2/catch_test_macros.hpp>

#include "wh/schema/stream/adapter.hpp"
#include "wh/schema/stream/reader/values_stream_reader.hpp"

TEST_CASE("stream adapter facade exposes filter-map transform and to-stream helpers",
          "[UT][wh/schema/stream/adapter.hpp][make_transform_stream_reader][condition][branch][boundary]") {
  auto filter_mapped = wh::schema::stream::make_filter_map_stream_reader(
      wh::schema::stream::make_values_stream_reader(
          std::vector<std::string>{"1", "skip", "2"}),
      [](const std::string &value)
          -> wh::core::result<wh::schema::stream::filter_map_step<int>> {
        if (value == "skip") {
          return wh::schema::stream::skip;
        }
        return std::stoi(value);
      });

  auto first = std::get<wh::schema::stream::stream_result<
      wh::schema::stream::stream_chunk<int>>>(filter_mapped.try_read());
  auto second = std::get<wh::schema::stream::stream_result<
      wh::schema::stream::stream_chunk<int>>>(filter_mapped.try_read());
  auto eof = std::get<wh::schema::stream::stream_result<
      wh::schema::stream::stream_chunk<int>>>(filter_mapped.try_read());
  REQUIRE(first.has_value());
  REQUIRE(first.value().value == std::optional<int>{1});
  REQUIRE(second.has_value());
  REQUIRE(second.value().value == std::optional<int>{2});
  REQUIRE(eof.has_value());
  REQUIRE(eof.value().is_terminal_eof());

  auto transformed = wh::schema::stream::make_transform_stream_reader(
      wh::schema::stream::make_values_stream_reader(std::vector<int>{3}),
      [](const int &value) -> wh::core::result<std::string> {
        return std::to_string(value * 2);
      });
  auto transformed_next = transformed.read();
  REQUIRE(transformed_next.has_value());
  REQUIRE(transformed_next.value().value == std::optional<std::string>{"6"});

  auto bridged = wh::schema::stream::make_to_stream_reader(
      wh::schema::stream::make_values_stream_reader(std::vector<int>{5}));
  auto bridged_next = bridged.read();
  REQUIRE(bridged_next.has_value());
  REQUIRE(bridged_next.value().value == std::optional<int>{5});
}

TEST_CASE("stream adapter facade propagates mapper failures through transform readers",
          "[UT][wh/schema/stream/adapter.hpp][make_filter_map_stream_reader][condition][branch][boundary][error]") {
  auto transformed = wh::schema::stream::make_transform_stream_reader(
      wh::schema::stream::make_values_stream_reader(std::vector<int>{1}),
      [](const int &) -> wh::core::result<std::string> {
        return wh::core::result<std::string>::failure(wh::core::errc::timeout);
      });
  auto failed = transformed.read();
  REQUIRE(failed.has_value());
  REQUIRE(failed.value().error == wh::core::errc::timeout);
  REQUIRE(transformed.is_closed());
}
