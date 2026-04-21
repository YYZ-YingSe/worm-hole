#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "wh/schema/stream/core/stream_base.hpp"

namespace {

using chunk_t = wh::schema::stream::stream_chunk<int>;
using chunk_result_t = wh::schema::stream::stream_result<chunk_t>;
using chunk_try_result_t = wh::schema::stream::stream_try_result<chunk_t>;

struct scripted_stream : wh::schema::stream::stream_base<scripted_stream, int> {
  std::vector<chunk_result_t> reads{};
  std::size_t read_index{0U};
  std::vector<chunk_try_result_t> polls{};
  std::size_t poll_index{0U};
  int close_calls{0};
  bool closed{false};

  auto read_impl() -> chunk_result_t {
    if (read_index < reads.size()) {
      return reads[read_index++];
    }
    return chunk_t::make_eof();
  }

  auto try_read_impl() -> chunk_try_result_t {
    if (poll_index < polls.size()) {
      return polls[poll_index++];
    }
    return wh::schema::stream::stream_pending;
  }

  auto close_impl() -> wh::core::result<void> {
    ++close_calls;
    closed = true;
    return {};
  }

  auto is_closed_impl() const noexcept -> bool { return closed; }
};

} // namespace

TEST_CASE(
    "stream base read and borrowed read clear caches and forward terminal results",
    "[UT][wh/schema/stream/core/stream_base.hpp][stream_base::read_borrowed][branch][boundary]") {
  scripted_stream stream{};
  stream.reads.push_back(chunk_result_t{chunk_t::make_value(7)});
  stream.reads.push_back(chunk_result_t::failure(wh::core::errc::timeout));
  stream.reads.push_back(chunk_result_t{chunk_t::make_eof()});

  auto borrowed = stream.read_borrowed();
  REQUIRE(borrowed.has_value());
  REQUIRE(borrowed.value().value != nullptr);
  REQUIRE(*borrowed.value().value == 7);

  auto failed = stream.read_borrowed();
  REQUIRE(failed.has_error());
  REQUIRE(failed.error() == wh::core::errc::timeout);

  auto eof = stream.read();
  REQUIRE(eof.has_value());
  REQUIRE(eof.value().is_terminal_eof());
}

TEST_CASE("stream base try_read_borrowed preserves pending and close delegates to derived state",
          "[UT][wh/schema/stream/core/"
          "stream_base.hpp][stream_base::try_read_borrowed][condition][branch]") {
  scripted_stream stream{};
  stream.polls.push_back(wh::schema::stream::stream_pending);
  stream.polls.push_back(chunk_result_t{chunk_t::make_value(11)});
  stream.polls.push_back(chunk_result_t::failure(wh::core::errc::invalid_argument));

  auto pending = stream.try_read_borrowed();
  REQUIRE(std::holds_alternative<wh::schema::stream::stream_signal>(pending));
  REQUIRE(std::get<wh::schema::stream::stream_signal>(pending) ==
          wh::schema::stream::stream_pending);

  auto borrowed = stream.try_read_borrowed();
  REQUIRE(std::holds_alternative<
          wh::schema::stream::stream_result<wh::schema::stream::stream_chunk_view<int>>>(borrowed));
  const auto &borrowed_result =
      std::get<wh::schema::stream::stream_result<wh::schema::stream::stream_chunk_view<int>>>(
          borrowed);
  REQUIRE(borrowed_result.has_value());
  REQUIRE(borrowed_result.value().value != nullptr);
  REQUIRE(*borrowed_result.value().value == 11);

  auto failed = stream.try_read_borrowed();
  REQUIRE(std::holds_alternative<
          wh::schema::stream::stream_result<wh::schema::stream::stream_chunk_view<int>>>(failed));
  const auto &failed_result =
      std::get<wh::schema::stream::stream_result<wh::schema::stream::stream_chunk_view<int>>>(
          failed);
  REQUIRE(failed_result.has_error());
  REQUIRE(failed_result.error() == wh::core::errc::invalid_argument);

  REQUIRE_FALSE(stream.is_closed());
  auto close_status = stream.close();
  REQUIRE(close_status.has_value());
  REQUIRE(stream.is_closed());
  REQUIRE(stream.close_calls == 1);
}
