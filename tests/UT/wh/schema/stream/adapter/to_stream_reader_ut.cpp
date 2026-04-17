#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>
#include <variant>

#include <stdexec/execution.hpp>

#include "helper/sender_capture.hpp"
#include "wh/schema/stream/adapter/to_stream_reader.hpp"
#include "wh/schema/stream/reader/values_stream_reader.hpp"

namespace {

struct throwing_reader_state {
  int read_count{0};
  int close_count{0};
  bool closed{false};
};

class throwing_reader final
    : public wh::schema::stream::stream_base<throwing_reader, int> {
public:
  explicit throwing_reader(std::shared_ptr<throwing_reader_state> state)
      : state_(std::move(state)) {}

  [[nodiscard]] auto try_read_impl()
      -> wh::schema::stream::stream_try_result<
          wh::schema::stream::stream_chunk<int>> {
    if (!state_ || state_->closed) {
      return wh::schema::stream::stream_chunk<int>::make_eof();
    }
    ++state_->read_count;
    if (state_->read_count == 1) {
      return wh::schema::stream::stream_chunk<int>::make_value(7);
    }
    throw std::runtime_error{"boom"};
  }

  [[nodiscard]] auto read_impl()
      -> wh::schema::stream::stream_result<
          wh::schema::stream::stream_chunk<int>> {
    auto next = try_read_impl();
    return std::get<wh::schema::stream::stream_result<
        wh::schema::stream::stream_chunk<int>>>(std::move(next));
  }

  auto close_impl() -> wh::core::result<void> {
    if (!state_->closed) {
      state_->closed = true;
      ++state_->close_count;
    }
    return {};
  }

  [[nodiscard]] auto is_closed_impl() const noexcept -> bool {
    return !state_ || state_->closed;
  }

private:
  std::shared_ptr<throwing_reader_state> state_{};
};

class stopped_async_reader final
    : public wh::schema::stream::stream_base<stopped_async_reader, int> {
public:
  using chunk_t = wh::schema::stream::stream_chunk<int>;

  [[nodiscard]] auto read_impl() -> wh::schema::stream::stream_result<chunk_t> {
    return chunk_t::make_eof();
  }

  [[nodiscard]] auto try_read_impl()
      -> wh::schema::stream::stream_try_result<chunk_t> {
    return wh::schema::stream::stream_result<chunk_t>{chunk_t::make_eof()};
  }

  [[nodiscard]] auto read_async() const { return stdexec::just_stopped(); }

  auto close_impl() -> wh::core::result<void> { return {}; }

  [[nodiscard]] auto is_closed_impl() const noexcept -> bool { return false; }
};

} // namespace

TEST_CASE("to stream reader converts thrown reads into error chunks and supports disabling automatic close",
          "[UT][wh/schema/stream/adapter/to_stream_reader.hpp][make_to_stream_reader][branch][boundary]") {
  auto state = std::make_shared<throwing_reader_state>();
  auto reader = wh::schema::stream::make_to_stream_reader(throwing_reader{state});

  auto first = std::get<wh::schema::stream::stream_result<
      wh::schema::stream::stream_chunk<int>>>(reader.try_read());
  REQUIRE(first.has_value());
  REQUIRE(first.value().value == std::optional<int>{7});

  auto second = std::get<wh::schema::stream::stream_result<
      wh::schema::stream::stream_chunk<int>>>(reader.try_read());
  REQUIRE(second.has_value());
  REQUIRE(second.value().error.failed());
  REQUIRE(second.value().error.code() == wh::core::errc::internal_error);
  REQUIRE(state->close_count == 1);

  auto third = std::get<wh::schema::stream::stream_result<
      wh::schema::stream::stream_chunk<int>>>(reader.try_read());
  REQUIRE(third.has_value());
  REQUIRE(third.value().is_terminal_eof());

  auto disabled_state = std::make_shared<throwing_reader_state>();
  auto disabled =
      wh::schema::stream::make_to_stream_reader(throwing_reader{disabled_state});
  disabled.set_automatic_close(wh::schema::stream::auto_close_disabled);

  auto disabled_first = std::get<wh::schema::stream::stream_result<
      wh::schema::stream::stream_chunk<int>>>(disabled.try_read());
  auto disabled_second = std::get<wh::schema::stream::stream_result<
      wh::schema::stream::stream_chunk<int>>>(disabled.try_read());
  REQUIRE(disabled_first.has_value());
  REQUIRE(disabled_second.has_value());
  REQUIRE(disabled_second.value().error.failed());
  REQUIRE(disabled_state->close_count == 0);
  REQUIRE(disabled.close().has_value());
  REQUIRE(disabled_state->close_count == 1);
}

TEST_CASE("to stream reader async read preserves stopped completion",
          "[UT][wh/schema/stream/adapter/to_stream_reader.hpp][to_stream_reader::read_async][condition][concurrency][branch]") {
  auto reader = wh::schema::stream::make_to_stream_reader(stopped_async_reader{});
  using result_t =
      wh::schema::stream::stream_result<wh::schema::stream::stream_chunk<int>>;

  wh::testing::helper::sender_capture<result_t> capture{};
  auto operation = stdexec::connect(
      reader.read_async(),
      wh::testing::helper::sender_capture_receiver<result_t>{&capture});
  stdexec::start(operation);

  REQUIRE(capture.ready.try_acquire());
  REQUIRE(capture.terminal == wh::testing::helper::sender_terminal_kind::stopped);
}

TEST_CASE("to stream async sender keeps state alive after wrapper destruction",
          "[UT][wh/schema/stream/adapter/to_stream_reader.hpp][to_stream_reader::read_async][lifetime][concurrency]") {
  auto sender = []() {
    auto reader = wh::schema::stream::make_to_stream_reader(
        wh::schema::stream::make_values_stream_reader(std::vector<int>{13}));
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
  REQUIRE(capture.value->value().value == std::optional<int>{13});
}
