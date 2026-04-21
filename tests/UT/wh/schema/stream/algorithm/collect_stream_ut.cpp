#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "wh/schema/stream/algorithm/collect_stream.hpp"
#include "wh/schema/stream/core/types.hpp"

namespace {

template <typename value_t> struct scripted_reader {
  using value_type = value_t;
  using chunk_type = wh::schema::stream::stream_chunk<value_t>;

  std::vector<wh::schema::stream::stream_result<chunk_type>> reads{};
  std::size_t index{0U};
  wh::core::result<void> close_status{};
  bool closed{false};

  [[nodiscard]] auto read() -> wh::schema::stream::stream_result<chunk_type> {
    if (index < reads.size()) {
      return reads[index++];
    }
    closed = true;
    return chunk_type::make_eof();
  }

  [[nodiscard]] auto try_read() -> wh::schema::stream::stream_try_result<chunk_type> {
    return read();
  }

  auto close() -> wh::core::result<void> {
    closed = true;
    return close_status;
  }

  [[nodiscard]] auto is_closed() const noexcept -> bool { return closed; }
};

} // namespace

TEST_CASE("collect stream detail drains source eof skips and callback failures while public "
          "collectors propagate read and close errors",
          "[UT][wh/schema/stream/algorithm/"
          "collect_stream.hpp][collect_stream_reader][condition][branch][boundary]") {
  using chunk_t = wh::schema::stream::stream_chunk<int>;

  scripted_reader<int> detail_reader{};
  auto source_eof = chunk_t::make_source_eof("lane-a");
  detail_reader.reads = {
      chunk_t::make_value(1),
      source_eof,
      chunk_t::make_value(2),
      chunk_t::make_eof(),
  };

  std::vector<int> drained{};
  auto drained_status = wh::schema::stream::detail::drain_stream(
      detail_reader, [&drained](int value) -> wh::core::result<void> {
        drained.push_back(value);
        if (value == 2) {
          return wh::core::result<void>::failure(wh::core::errc::canceled);
        }
        return {};
      });
  REQUIRE(drained_status.has_error());
  REQUIRE(drained_status.error() == wh::core::errc::canceled);
  REQUIRE(drained == std::vector<int>({1, 2}));

  scripted_reader<int> collect_reader{};
  auto error_chunk = chunk_t{};
  error_chunk.error = wh::core::errc::timeout;
  collect_reader.reads = {
      chunk_t::make_value(3),
      error_chunk,
  };
  auto collected = wh::schema::stream::collect_stream_reader(std::move(collect_reader));
  REQUIRE(collected.has_error());
  REQUIRE(collected.error() == wh::core::errc::timeout);

  scripted_reader<std::string> text_reader{};
  text_reader.reads = {
      wh::schema::stream::stream_chunk<std::string>::make_value("a"),
      wh::schema::stream::stream_chunk<std::string>::make_value("b"),
      wh::schema::stream::stream_chunk<std::string>::make_eof(),
  };
  text_reader.close_status = wh::core::result<void>::failure(wh::core::errc::internal_error);
  auto text = wh::schema::stream::collect_text_stream_reader(std::move(text_reader));
  REQUIRE(text.has_error());
  REQUIRE(text.error() == wh::core::errc::internal_error);
}

TEST_CASE("collect stream public helpers gather happy-path values and text",
          "[UT][wh/schema/stream/algorithm/"
          "collect_stream.hpp][collect_text_stream_reader][condition][branch][boundary]") {
  scripted_reader<int> value_reader{};
  value_reader.reads = {
      wh::schema::stream::stream_chunk<int>::make_value(4),
      wh::schema::stream::stream_chunk<int>::make_value(5),
      wh::schema::stream::stream_chunk<int>::make_eof(),
  };
  auto values = wh::schema::stream::collect_stream_reader(std::move(value_reader));
  REQUIRE(values.has_value());
  REQUIRE(values.value() == std::vector<int>({4, 5}));

  scripted_reader<std::string> text_reader{};
  text_reader.reads = {
      wh::schema::stream::stream_chunk<std::string>::make_value("ab"),
      wh::schema::stream::stream_chunk<std::string>::make_value("cd"),
      wh::schema::stream::stream_chunk<std::string>::make_eof(),
  };
  auto text = wh::schema::stream::collect_text_stream_reader(std::move(text_reader));
  REQUIRE(text.has_value());
  REQUIRE(text.value() == "abcd");
}
