#include <array>
#include <list>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "helper/test_thread_wait.hpp"
#include "wh/core/any.hpp"
#include "wh/schema/stream/reader/values_stream_reader.hpp"

TEST_CASE(
    "values stream reader factories and range concept cover empty single and generic range cases",
    "[UT][wh/schema/stream/reader/"
    "values_stream_reader.hpp][make_values_stream_reader][condition][branch][boundary]") {
  static_assert(wh::schema::stream::values_stream_range<std::vector<int>>);
  static_assert(wh::schema::stream::values_stream_range<std::list<int>>);

  auto empty = wh::schema::stream::make_empty_stream_reader<int>();
  auto empty_first = empty.read();
  auto empty_second = empty.read();
  REQUIRE(empty_first.has_value());
  REQUIRE(empty_first.value().is_terminal_eof());
  REQUIRE(empty_second.has_value());
  REQUIRE(empty_second.value().is_terminal_eof());

  auto single = wh::schema::stream::make_single_value_stream_reader<int>(9);
  auto single_value = single.read();
  auto single_eof = single.read();
  REQUIRE(single_value.has_value());
  REQUIRE(single_value.value().value == std::optional<int>{9});
  REQUIRE(single_eof.has_value());
  REQUIRE(single_eof.value().is_terminal_eof());

  auto reader = wh::schema::stream::make_values_stream_reader(std::list<int>{1, 2, 3});
  static_assert(
      std::same_as<decltype(reader), wh::schema::stream::values_stream_reader<std::list<int>>>);

  auto first = reader.read();
  auto second = reader.read();
  auto third = reader.read();
  auto eof = reader.read();
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  REQUIRE(third.has_value());
  REQUIRE(eof.has_value());
  REQUIRE(first.value().value == std::optional<int>{1});
  REQUIRE(second.value().value == std::optional<int>{2});
  REQUIRE(third.value().value == std::optional<int>{3});
  REQUIRE(eof.value().is_terminal_eof());
}

TEST_CASE("values stream reader move close and async paths preserve cursor and closure state",
          "[UT][wh/schema/stream/reader/"
          "values_stream_reader.hpp][values_stream_reader::read_async][branch][boundary]") {
  auto source = wh::schema::stream::make_values_stream_reader(std::vector<int>{4, 5, 6});
  auto first = source.read();
  REQUIRE(first.has_value());
  REQUIRE(first.value().value == std::optional<int>{4});

  auto moved = std::move(source);
  auto second = moved.read();
  REQUIRE(second.has_value());
  REQUIRE(second.value().value == std::optional<int>{5});

  auto assigned = wh::schema::stream::make_values_stream_reader(std::vector<int>{8, 9});
  assigned = std::move(moved);
  auto third = assigned.read();
  REQUIRE(third.has_value());
  REQUIRE(third.value().value == std::optional<int>{6});
  REQUIRE(assigned.is_source_closed());

  auto async_reader = wh::schema::stream::make_values_stream_reader(std::vector<int>{11});
  auto async_result = wh::testing::helper::wait_value_on_test_thread(async_reader.read_async());
  REQUIRE(async_result.has_value());
  REQUIRE(async_result.value().value == std::optional<int>{11});

  auto close_reader = wh::schema::stream::make_values_stream_reader(std::vector<int>{21, 22});
  REQUIRE_FALSE(close_reader.is_closed());
  REQUIRE(close_reader.close().has_value());
  REQUIRE(close_reader.is_closed());
  auto closed_read = close_reader.read();
  REQUIRE(closed_read.has_value());
  REQUIRE(closed_read.value().is_terminal_eof());
}

TEST_CASE("values stream reader preserves vector<any> elements instead of wrapping the whole range",
          "[UT][wh/schema/stream/reader/"
          "values_stream_reader.hpp][make_values_stream_reader][any][boundary]") {
  std::vector<wh::core::any> values{};
  values.emplace_back(7);
  values.emplace_back(8);

  auto reader = wh::schema::stream::make_values_stream_reader(std::move(values));
  auto first = reader.read();
  auto second = reader.read();
  auto eof = reader.read();

  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  REQUIRE(eof.has_value());
  REQUIRE(first.value().value.has_value());
  REQUIRE(second.value().value.has_value());
  REQUIRE(eof.value().is_terminal_eof());
  REQUIRE(*wh::core::any_cast<int>(&*first.value().value) == 7);
  REQUIRE(*wh::core::any_cast<int>(&*second.value().value) == 8);
}
