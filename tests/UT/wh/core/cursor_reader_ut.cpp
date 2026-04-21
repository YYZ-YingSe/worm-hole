#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "wh/core/cursor_reader.hpp"
#include "wh/schema/stream/pipe.hpp"

TEST_CASE("cursor reader facade exports top level reader and factory",
          "[UT][wh/core/cursor_reader.hpp][make_cursor_readers][condition][branch][boundary]") {
  auto [writer, source] = wh::schema::stream::make_pipe_stream<int>(2U);
  REQUIRE(writer.try_write(5).has_value());
  REQUIRE(writer.close().has_value());

  using source_t = decltype(source);
  using cursor_t = wh::core::cursor_reader<source_t>;
  STATIC_REQUIRE(std::is_default_constructible_v<cursor_t>);

  auto readers = wh::core::make_cursor_readers(std::move(source), 1U);
  REQUIRE(readers.size() == 1U);
  auto value = readers.front().read();
  REQUIRE(value.has_value());
  REQUIRE(value.value().value.has_value());
  REQUIRE(*value.value().value == 5);
}

TEST_CASE(
    "cursor reader facade supports zero-reader boundary and default constructible cursor type",
    "[UT][wh/core/cursor_reader.hpp][cursor_reader][condition][boundary]") {
  auto [writer, source] = wh::schema::stream::make_pipe_stream<int>(1U);
  REQUIRE(writer.close().has_value());

  using source_t = decltype(source);
  using cursor_t = wh::core::cursor_reader<source_t>;
  STATIC_REQUIRE(std::is_default_constructible_v<cursor_t>);

  auto readers = wh::core::make_cursor_readers(std::move(source), 0U);
  REQUIRE(readers.empty());
}
