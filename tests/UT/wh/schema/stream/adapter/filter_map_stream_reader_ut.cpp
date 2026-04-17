#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <variant>

#include <stdexec/execution.hpp>

#include "helper/sender_capture.hpp"
#include "wh/schema/stream/adapter/filter_map_stream_reader.hpp"
#include "wh/schema/stream/reader/values_stream_reader.hpp"

namespace {

class invalid_borrowed_async_reader final
    : public wh::schema::stream::stream_base<invalid_borrowed_async_reader,
                                             std::string> {
public:
  using chunk_t = wh::schema::stream::stream_chunk<std::string>;
  using chunk_view_t = wh::schema::stream::stream_chunk_view<std::string>;

  [[nodiscard]] auto read_impl()
      -> wh::schema::stream::stream_result<chunk_t> {
    return chunk_t{};
  }

  [[nodiscard]] auto try_read_impl()
      -> wh::schema::stream::stream_try_result<chunk_t> {
    return wh::schema::stream::stream_result<chunk_t>{chunk_t{}};
  }

  [[nodiscard]] auto read_borrowed()
      -> wh::schema::stream::stream_result<chunk_view_t> {
    return chunk_view_t{};
  }

  [[nodiscard]] auto try_read_borrowed()
      -> wh::schema::stream::stream_try_result<chunk_view_t> {
    return wh::schema::stream::stream_result<chunk_view_t>{chunk_view_t{}};
  }

  [[nodiscard]] auto read_async() const { return stdexec::just_stopped(); }

  auto close_impl() -> wh::core::result<void> { return {}; }

  [[nodiscard]] auto is_closed_impl() const noexcept -> bool { return false; }
};

} // namespace

TEST_CASE("filter map stream reader skips mapped items and turns callback failures into terminal error chunks",
          "[UT][wh/schema/stream/adapter/filter_map_stream_reader.hpp][make_filter_map_stream_reader][branch][boundary]") {
  auto reader = wh::schema::stream::make_filter_map_stream_reader(
      wh::schema::stream::make_values_stream_reader(
          std::vector<std::string>{"1", "skip", "bad"}),
      [](const std::string &input)
          -> wh::core::result<wh::schema::stream::filter_map_step<int>> {
        if (input == "skip") {
          return wh::schema::stream::skip;
        }
        if (input == "bad") {
          return wh::core::result<
              wh::schema::stream::filter_map_step<int>>::failure(
              wh::core::errc::parse_error);
        }
        return std::stoi(input);
      });

  auto first = std::get<wh::schema::stream::stream_result<
      wh::schema::stream::stream_chunk<int>>>(reader.try_read());
  REQUIRE(first.has_value());
  REQUIRE(first.value().value == std::optional<int>{1});

  auto second = std::get<wh::schema::stream::stream_result<
      wh::schema::stream::stream_chunk<int>>>(reader.try_read());
  REQUIRE(second.has_value());
  REQUIRE(second.value().error == wh::core::errc::parse_error);
  REQUIRE(reader.is_closed());
}

TEST_CASE("filter map stream reader rejects missing borrowed payload and preserves stopped async completion",
          "[UT][wh/schema/stream/adapter/filter_map_stream_reader.hpp][filter_map_stream_reader::read_async][condition][concurrency][branch]") {
  auto invalid_reader = wh::schema::stream::make_filter_map_stream_reader(
      invalid_borrowed_async_reader{},
      [](const std::string &input)
          -> wh::core::result<wh::schema::stream::filter_map_step<int>> {
        return static_cast<int>(input.size());
      });

  auto invalid = invalid_reader.read();
  REQUIRE(invalid.has_error());
  REQUIRE(invalid.error() == wh::core::errc::protocol_error);
  REQUIRE(invalid_reader.is_closed());

  auto stopped_reader = wh::schema::stream::make_filter_map_stream_reader(
      invalid_borrowed_async_reader{},
      [](const std::string &input)
          -> wh::core::result<wh::schema::stream::filter_map_step<int>> {
        return static_cast<int>(input.size());
      });
  using result_t =
      wh::schema::stream::stream_result<wh::schema::stream::stream_chunk<int>>;

  wh::testing::helper::sender_capture<result_t> capture{};
  auto operation = stdexec::connect(
      stopped_reader.read_async(),
      wh::testing::helper::sender_capture_receiver<result_t>{&capture});
  stdexec::start(operation);

  REQUIRE(capture.ready.try_acquire());
  REQUIRE(capture.terminal == wh::testing::helper::sender_terminal_kind::stopped);
}

TEST_CASE("filter map stream async sender skips values and keeps state alive after wrapper destruction",
          "[UT][wh/schema/stream/adapter/filter_map_stream_reader.hpp][filter_map_stream_reader::read_async][lifetime][branch][concurrency]") {
  auto sender = []() {
    auto reader = wh::schema::stream::make_filter_map_stream_reader(
        wh::schema::stream::make_values_stream_reader(
            std::vector<std::string>{"skip", "7"}),
        [](const std::string &input)
            -> wh::core::result<wh::schema::stream::filter_map_step<int>> {
          if (input == "skip") {
            return wh::schema::stream::skip;
          }
          return std::stoi(input);
        });
    return reader.read_async();
  }();

  using result_t =
      wh::schema::stream::stream_result<wh::schema::stream::stream_chunk<int>>;
  wh::testing::helper::sender_capture<result_t> capture{};
  auto operation = stdexec::connect(
      std::move(sender),
      wh::testing::helper::sender_capture_receiver<result_t>{&capture});
  stdexec::start(operation);

  REQUIRE(capture.ready.try_acquire());
  REQUIRE(capture.terminal ==
          wh::testing::helper::sender_terminal_kind::value);
  REQUIRE(capture.value.has_value());
  REQUIRE(capture.value->has_value());
  REQUIRE(capture.value->value().value == std::optional<int>{7});
}

TEST_CASE("filter map stream reader forwards owned chunks to move-only callbacks",
          "[UT][wh/schema/stream/adapter/filter_map_stream_reader.hpp][make_filter_map_stream_reader][move_only][ownership]") {
  std::vector<std::unique_ptr<int>> values{};
  values.push_back(std::make_unique<int>(3));
  values.push_back(std::make_unique<int>(9));

  auto reader = wh::schema::stream::make_filter_map_stream_reader(
      wh::schema::stream::make_values_stream_reader(std::move(values)),
      [](std::unique_ptr<int> value)
          -> wh::core::result<wh::schema::stream::filter_map_step<int>> {
        if (value == nullptr) {
          return wh::core::result<
              wh::schema::stream::filter_map_step<int>>::failure(
              wh::core::errc::invalid_argument);
        }
        if (*value == 3) {
          return wh::schema::stream::skip;
        }
        return *value + 1;
      });

  auto first = std::get<wh::schema::stream::stream_result<
      wh::schema::stream::stream_chunk<int>>>(reader.try_read());
  REQUIRE(first.has_value());
  REQUIRE(first.value().value == std::optional<int>{10});

  auto eof = std::get<wh::schema::stream::stream_result<
      wh::schema::stream::stream_chunk<int>>>(reader.try_read());
  REQUIRE(eof.has_value());
  REQUIRE(eof.value().eof);
}
