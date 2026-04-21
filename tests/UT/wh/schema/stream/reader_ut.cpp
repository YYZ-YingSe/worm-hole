#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "wh/schema/stream/reader.hpp"

TEST_CASE("stream reader facade exports value readers through public header",
          "[UT][wh/schema/stream/reader.hpp][make_values_stream_reader][branch]") {
  auto reader = wh::schema::stream::make_values_stream_reader(std::vector<int>{1, 2});

  auto first = reader.read();
  REQUIRE(first.has_value());
  REQUIRE(first.value().value == std::optional<int>{1});

  auto second = reader.read();
  REQUIRE(second.has_value());
  REQUIRE(second.value().value == std::optional<int>{2});

  auto eof = reader.read();
  REQUIRE(eof.has_value());
  REQUIRE(eof.value().is_terminal_eof());
}

TEST_CASE("stream reader facade exports retained copy readers through public header",
          "[UT][wh/schema/stream/reader.hpp][make_copy_stream_readers][condition][boundary]") {
  auto none = wh::schema::stream::make_copy_stream_readers(
      wh::schema::stream::make_values_stream_reader(std::vector<int>{1}), 0U);
  REQUIRE(none.empty());

  auto copies = wh::schema::stream::make_copy_stream_readers(
      wh::schema::stream::make_values_stream_reader(std::vector<int>{4, 5}), 2U);
  REQUIRE(copies.size() == 2U);

  for (auto &copy : copies) {
    auto first = copy.read();
    REQUIRE(first.has_value());
    REQUIRE(first.value().value == std::optional<int>{4});

    auto second = copy.read();
    REQUIRE(second.has_value());
    REQUIRE(second.value().value == std::optional<int>{5});

    auto eof = copy.read();
    REQUIRE(eof.has_value());
    REQUIRE(eof.value().is_terminal_eof());
  }
}
