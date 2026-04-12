#include <catch2/catch_test_macros.hpp>

#include <sstream>

#include "wh/schema/stream/core.hpp"

namespace {

struct core_facade_reader {
  using value_type = int;

  auto read()
      -> wh::schema::stream::stream_result<wh::schema::stream::stream_chunk<int>> {
    return wh::schema::stream::stream_chunk<int>::make_eof();
  }

  auto try_read()
      -> wh::schema::stream::stream_try_result<wh::schema::stream::stream_chunk<int>> {
    return wh::schema::stream::stream_result<wh::schema::stream::stream_chunk<int>>{
        wh::schema::stream::stream_chunk<int>::make_eof()};
  }

  auto close() -> wh::core::result<void> { return {}; }

  auto is_closed() const -> bool { return false; }
};

} // namespace

TEST_CASE("stream core facade exposes signals chunks and borrow helpers",
          "[UT][wh/schema/stream/core.hpp][stream_chunk][branch][boundary]") {
  REQUIRE(wh::schema::stream::to_string(wh::schema::stream::stream_signal::pending) ==
          "pending");
  std::ostringstream stream{};
  stream << wh::schema::stream::stream_signal::pending;
  REQUIRE(stream.str() == "pending");

  auto value_chunk = wh::schema::stream::stream_chunk<int>::make_value(7);
  auto eof_chunk = wh::schema::stream::stream_chunk<int>::make_eof();
  auto source_eof =
      wh::schema::stream::stream_chunk<int>::make_source_eof("source-a");

  REQUIRE(value_chunk.value == std::optional<int>{7});
  REQUIRE(eof_chunk.is_terminal_eof());
  REQUIRE(source_eof.is_source_eof());

  auto view = wh::schema::stream::borrow_chunk_until_next(value_chunk);
  REQUIRE(view.value != nullptr);
  REQUIRE(*view.value == 7);

  auto materialized = wh::schema::stream::materialize_chunk(view);
  REQUIRE(materialized.value == std::optional<int>{7});
}

TEST_CASE("stream core facade reexports reader concepts and auto-close flags",
          "[UT][wh/schema/stream/core.hpp][stream_reader][condition][boundary]") {
  STATIC_REQUIRE(wh::schema::stream::stream_reader<core_facade_reader>);
  STATIC_REQUIRE(
      !wh::schema::stream::borrowed_stream_reader<core_facade_reader>);

  REQUIRE(wh::schema::stream::auto_close_enabled.enabled);
  REQUIRE_FALSE(wh::schema::stream::auto_close_disabled.enabled);
}
