#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "wh/schema/stream/core/types.hpp"

TEST_CASE("stream types build value eof and source eof chunks and preserve borrowed conversion",
          "[UT][wh/schema/stream/core/types.hpp][stream_chunk::make_value][branch][boundary]") {
  using chunk_t = wh::schema::stream::stream_chunk<int>;
  using view_t = wh::schema::stream::stream_chunk_view<int>;

  REQUIRE(wh::schema::stream::auto_close_enabled.enabled);
  REQUIRE_FALSE(wh::schema::stream::auto_close_disabled.enabled);

  auto value_chunk = chunk_t::make_value(7);
  value_chunk.source = "left";
  auto eof_chunk = chunk_t::make_eof();
  auto source_eof = chunk_t::make_source_eof("branch-a");

  REQUIRE(value_chunk.value == std::optional<int>{7});
  REQUIRE_FALSE(value_chunk.eof);
  REQUIRE_FALSE(value_chunk.is_terminal_eof());
  REQUIRE_FALSE(value_chunk.is_source_eof());
  REQUIRE(eof_chunk.is_terminal_eof());
  REQUIRE_FALSE(eof_chunk.is_source_eof());
  REQUIRE(source_eof.is_source_eof());
  REQUIRE_FALSE(source_eof.is_terminal_eof());
  REQUIRE(source_eof.source == "branch-a");

  auto borrowed = wh::schema::stream::borrow_chunk_until_next(value_chunk);
  REQUIRE(borrowed.value != nullptr);
  REQUIRE(*borrowed.value == 7);
  REQUIRE(borrowed.source == "left");
  REQUIRE_FALSE(borrowed.eof);

  auto materialized = wh::schema::stream::materialize_chunk(borrowed);
  REQUIRE(materialized.value == std::optional<int>{7});
  REQUIRE(materialized.source == "left");
  REQUIRE_FALSE(materialized.eof);

  auto value_view = view_t::make_value(*value_chunk.value);
  auto eof_view = view_t::make_eof();
  value_view.source = "view-a";
  REQUIRE(value_view.value != nullptr);
  REQUIRE(*value_view.value == 7);
  REQUIRE_FALSE(value_view.is_terminal_eof());
  REQUIRE(eof_view.is_terminal_eof());
  REQUIRE_FALSE(eof_view.is_source_eof());
}

TEST_CASE("stream types materialize null borrowed value and preserve error eof metadata",
          "[UT][wh/schema/stream/core/types.hpp][materialize_chunk][boundary][condition]") {
  wh::schema::stream::stream_chunk_view<int> view{};
  view.error = wh::core::errc::timeout;
  view.eof = true;
  view.source = "upstream";

  auto materialized = wh::schema::stream::materialize_chunk(view);
  REQUIRE_FALSE(materialized.value.has_value());
  REQUIRE(materialized.error == wh::core::errc::timeout);
  REQUIRE(materialized.eof);
  REQUIRE(materialized.source == "upstream");
  REQUIRE(materialized.is_source_eof());
}
