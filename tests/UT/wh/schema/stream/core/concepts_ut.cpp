#include <variant>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "wh/schema/stream/core/concepts.hpp"
#include "wh/schema/stream/core/status.hpp"
#include "wh/schema/stream/core/types.hpp"

namespace {

using chunk_t = wh::schema::stream::stream_chunk<int>;
using chunk_result_t = wh::schema::stream::stream_result<chunk_t>;
using chunk_try_result_t = wh::schema::stream::stream_try_result<chunk_t>;
using chunk_view_t = wh::schema::stream::stream_chunk_view<int>;
using chunk_view_result_t = wh::schema::stream::stream_result<chunk_view_t>;
using chunk_view_try_result_t = wh::schema::stream::stream_try_result<chunk_view_t>;

struct basic_reader {
  using value_type = int;

  auto read() -> chunk_result_t { return chunk_t::make_eof(); }
  auto try_read() -> chunk_try_result_t { return chunk_t::make_eof(); }
  auto close() -> wh::core::result<void> { return {}; }
  auto is_closed() const -> bool { return false; }
};

struct borrowed_reader : basic_reader {
  auto read_borrowed() -> chunk_view_result_t { return chunk_view_t::make_eof(); }
  auto try_read_borrowed() -> chunk_view_try_result_t { return chunk_view_t::make_eof(); }
};

struct async_reader : basic_reader {
  auto read_async() { return stdexec::just(chunk_result_t{chunk_t::make_eof()}); }
};

struct missing_close_reader {
  using value_type = int;

  auto read() -> chunk_result_t { return chunk_t::make_eof(); }
  auto try_read() -> chunk_try_result_t { return chunk_t::make_eof(); }
  auto is_closed() const -> bool { return false; }
};

static_assert(wh::schema::stream::stream_reader<basic_reader>);
static_assert(!wh::schema::stream::borrowed_stream_reader<basic_reader>);
static_assert(wh::schema::stream::borrowed_stream_reader<borrowed_reader>);
static_assert(wh::schema::stream::detail::async_stream_reader<async_reader>);
static_assert(!wh::schema::stream::stream_reader<missing_close_reader>);
static_assert(!wh::schema::stream::detail::async_stream_reader<basic_reader>);

} // namespace

TEST_CASE("stream concepts distinguish basic borrowed and async readers",
          "[UT][wh/schema/stream/core/concepts.hpp][stream_reader][condition][branch]") {
  SUCCEED();
}

TEST_CASE("stream concepts reject boundary shapes that are not full readers",
          "[UT][wh/schema/stream/core/concepts.hpp][borrowed_stream_reader][boundary]") {
  STATIC_REQUIRE(!wh::schema::stream::stream_reader<int>);
  STATIC_REQUIRE(!wh::schema::stream::borrowed_stream_reader<missing_close_reader>);
  STATIC_REQUIRE(!wh::schema::stream::detail::async_stream_reader<borrowed_reader>);
  SUCCEED();
}
