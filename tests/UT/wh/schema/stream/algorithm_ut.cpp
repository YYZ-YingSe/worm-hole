#include <catch2/catch_test_macros.hpp>

#include "wh/schema/stream/algorithm.hpp"

namespace {

template <typename value_t> struct test_chunk {
  std::optional<value_t> value{};
  wh::core::error_code error{};
  bool eof{false};
  bool source_eof{false};

  [[nodiscard]] static auto make_value(value_t next) -> test_chunk {
    test_chunk chunk{};
    chunk.value = std::move(next);
    return chunk;
  }

  [[nodiscard]] static auto make_eof() -> test_chunk {
    test_chunk chunk{};
    chunk.eof = true;
    return chunk;
  }

  [[nodiscard]] auto is_terminal_eof() const noexcept -> bool {
    return eof && !source_eof;
  }

  [[nodiscard]] auto is_source_eof() const noexcept -> bool { return source_eof; }
};

template <typename value_t> struct test_reader {
  using value_type = value_t;
  using chunk_type = test_chunk<value_t>;

  std::vector<value_t> values{};
  std::size_t index{0U};
  bool closed{false};

  [[nodiscard]] auto read() -> wh::core::result<chunk_type> {
    if (closed || index >= values.size()) {
      closed = true;
      return chunk_type::make_eof();
    }
    return chunk_type::make_value(std::move(values[index++]));
  }

  [[nodiscard]] auto try_read() -> wh::core::result<chunk_type> {
    return read();
  }

  auto close() -> wh::core::result<void> {
    closed = true;
    return {};
  }

  [[nodiscard]] auto is_closed() const noexcept -> bool { return closed; }
};

} // namespace

TEST_CASE("stream algorithm facade collects values and concatenates text",
          "[UT][wh/schema/stream/algorithm.hpp][collect_stream_reader][branch][boundary]") {
  test_reader<int> values_reader{{1, 2, 3}};
  auto collected =
      wh::schema::stream::collect_stream_reader(std::move(values_reader));
  REQUIRE(collected.has_value());
  REQUIRE(collected.value() == std::vector<int>({1, 2, 3}));

  test_reader<std::string> text_reader{{"a", "b", "c"}};
  auto text = wh::schema::stream::collect_text_stream_reader(std::move(text_reader));
  REQUIRE(text.has_value());
  REQUIRE(text.value() == "abc");
}

TEST_CASE("stream algorithm facade preserves empty collection boundaries",
          "[UT][wh/schema/stream/algorithm.hpp][collect_text_stream_reader][condition][boundary]") {
  test_reader<int> empty_values{};
  auto collected =
      wh::schema::stream::collect_stream_reader(std::move(empty_values));
  REQUIRE(collected.has_value());
  REQUIRE(collected.value().empty());

  test_reader<std::string> empty_text{};
  auto text =
      wh::schema::stream::collect_text_stream_reader(std::move(empty_text));
  REQUIRE(text.has_value());
  REQUIRE(text.value().empty());
}
