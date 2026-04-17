#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <variant>

#include <stdexec/execution.hpp>

#include "helper/sender_capture.hpp"
#include "wh/schema/stream/adapter/transform_stream_reader.hpp"
#include "wh/schema/stream/reader/values_stream_reader.hpp"

namespace {

class invalid_async_reader final
    : public wh::schema::stream::stream_base<invalid_async_reader, std::string> {
public:
  using chunk_t = wh::schema::stream::stream_chunk<std::string>;

  [[nodiscard]] auto read_impl()
      -> wh::schema::stream::stream_result<chunk_t> {
    return chunk_t{};
  }

  [[nodiscard]] auto try_read_impl()
      -> wh::schema::stream::stream_try_result<chunk_t> {
    return wh::schema::stream::stream_result<chunk_t>{chunk_t{}};
  }

  [[nodiscard]] auto read_async() const { return stdexec::just_stopped(); }

  auto close_impl() -> wh::core::result<void> { return {}; }

  [[nodiscard]] auto is_closed_impl() const noexcept -> bool { return false; }
};

} // namespace

TEST_CASE("transform stream reader maps values and converts callback failures into terminal error chunks",
          "[UT][wh/schema/stream/adapter/transform_stream_reader.hpp][make_transform_stream_reader][branch][boundary]") {
  auto reader = wh::schema::stream::make_transform_stream_reader(
      wh::schema::stream::make_values_stream_reader(
          std::vector<std::string>{"3", "skip"}),
      [](const std::string &input) -> wh::core::result<int> {
        if (input == "skip") {
          return wh::core::result<int>::failure(wh::core::errc::queue_empty);
        }
        return std::stoi(input);
      });

  auto first = std::get<wh::schema::stream::stream_result<
      wh::schema::stream::stream_chunk<int>>>(reader.try_read());
  REQUIRE(first.has_value());
  REQUIRE(first.value().value == std::optional<int>{3});

  auto second = std::get<wh::schema::stream::stream_result<
      wh::schema::stream::stream_chunk<int>>>(reader.try_read());
  REQUIRE(second.has_value());
  REQUIRE(second.value().error == wh::core::errc::queue_empty);
  REQUIRE(reader.is_closed());
}

TEST_CASE("transform stream reader rejects chunks without payload and preserves stopped async completion",
          "[UT][wh/schema/stream/adapter/transform_stream_reader.hpp][transform_stream_reader::read_async][condition][concurrency][branch]") {
  auto invalid_reader = wh::schema::stream::make_transform_stream_reader(
      invalid_async_reader{},
      [](const std::string &input) -> wh::core::result<int> {
        return static_cast<int>(input.size());
      });

  auto invalid = invalid_reader.read();
  REQUIRE(invalid.has_error());
  REQUIRE(invalid.error() == wh::core::errc::protocol_error);
  REQUIRE(invalid_reader.is_closed());

  auto stopped_reader = wh::schema::stream::make_transform_stream_reader(
      invalid_async_reader{},
      [](const std::string &input) -> wh::core::result<int> {
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

TEST_CASE("transform stream async sender keeps state alive after wrapper destruction",
          "[UT][wh/schema/stream/adapter/transform_stream_reader.hpp][transform_stream_reader::read_async][lifetime][concurrency]") {
  auto sender = []() {
    auto reader = wh::schema::stream::make_transform_stream_reader(
        wh::schema::stream::make_values_stream_reader(
            std::vector<std::string>{"11"}),
        [](const std::string &input) -> wh::core::result<int> {
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
  REQUIRE(capture.value->value().value == std::optional<int>{11});
}

TEST_CASE("transform stream reader forwards owned chunks as movable inputs when source is not borrowed",
          "[UT][wh/schema/stream/adapter/transform_stream_reader.hpp][make_transform_stream_reader][move_only][ownership]") {
  std::vector<std::unique_ptr<int>> values{};
  values.push_back(std::make_unique<int>(7));

  auto reader = wh::schema::stream::make_transform_stream_reader(
      wh::schema::stream::make_values_stream_reader(std::move(values)),
      [](std::unique_ptr<int> value) -> wh::core::result<int> {
        if (value == nullptr) {
          return wh::core::result<int>::failure(wh::core::errc::invalid_argument);
        }
        return *value + 5;
      });

  auto first = reader.read();
  REQUIRE(first.has_value());
  REQUIRE(first.value().value == std::optional<int>{12});

  auto eof = reader.read();
  REQUIRE(eof.has_value());
  REQUIRE(eof.value().eof);
}
