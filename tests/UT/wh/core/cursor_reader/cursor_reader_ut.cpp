#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <optional>
#include <stop_token>
#include <thread>
#include <tuple>
#include <type_traits>

#include <stdexec/execution.hpp>

#include "helper/sender_env.hpp"
#include "helper/static_thread_scheduler.hpp"
#include "wh/core/cursor_reader/cursor_reader.hpp"
#include "wh/schema/stream/pipe.hpp"

namespace {

template <typename result_t> struct cursor_read_receiver_state {
  bool value_called{false};
  bool error_called{false};
  bool stopped_called{false};
  std::optional<result_t> value{};
  std::exception_ptr error{};
};

template <typename scheduler_t = stdexec::inline_scheduler>
using cursor_read_receiver_env =
    wh::testing::helper::scheduler_env<scheduler_t, std::stop_token>;

template <typename result_t, typename scheduler_t = stdexec::inline_scheduler>
struct cursor_read_receiver {
  using receiver_concept = stdexec::receiver_t;

  cursor_read_receiver_state<result_t> *state{nullptr};
  cursor_read_receiver_env<scheduler_t> env{};

  auto set_value(result_t value) noexcept -> void {
    state->value_called = true;
    state->value.emplace(std::move(value));
  }

  auto set_error(std::exception_ptr error) noexcept -> void {
    state->error_called = true;
    state->error = std::move(error);
  }

  auto set_stopped() noexcept -> void { state->stopped_called = true; }

  [[nodiscard]] auto get_env() const noexcept
      -> cursor_read_receiver_env<scheduler_t> {
    return env;
  }
};

struct retained_cursor_probe {
  static inline std::atomic<int> live_count{0};

  int value{0};

  retained_cursor_probe() noexcept { live_count.fetch_add(1, std::memory_order_relaxed); }
  explicit retained_cursor_probe(const int next) noexcept : value(next) {
    live_count.fetch_add(1, std::memory_order_relaxed);
  }
  retained_cursor_probe(const retained_cursor_probe &other) noexcept
      : value(other.value) {
    live_count.fetch_add(1, std::memory_order_relaxed);
  }
  retained_cursor_probe(retained_cursor_probe &&other) noexcept
      : value(other.value) {
    live_count.fetch_add(1, std::memory_order_relaxed);
  }
  auto operator=(const retained_cursor_probe &) -> retained_cursor_probe & = default;
  auto operator=(retained_cursor_probe &&) noexcept -> retained_cursor_probe & = default;
  ~retained_cursor_probe() {
    live_count.fetch_sub(1, std::memory_order_relaxed);
  }
};

} // namespace

TEST_CASE("cursor reader retains one source for many fixed cursors",
          "[UT][wh/core/cursor_reader/cursor_reader.hpp][make_readers][branch][boundary]") {
  auto [writer, source] = wh::schema::stream::make_pipe_stream<std::string>(8U);
  REQUIRE(writer.try_write("alpha").has_value());
  REQUIRE(writer.try_write("beta").has_value());
  REQUIRE(writer.try_write("gamma").has_value());
  REQUIRE(writer.close().has_value());

  auto empty = wh::core::cursor_reader<decltype(source)>::make_readers(
      std::move(source), 0U);
  REQUIRE(empty.empty());

  auto [writer_two, source_two] =
      wh::schema::stream::make_pipe_stream<std::string>(8U);
  REQUIRE(writer_two.try_write("alpha").has_value());
  REQUIRE(writer_two.try_write("beta").has_value());
  REQUIRE(writer_two.try_write("gamma").has_value());
  REQUIRE(writer_two.close().has_value());

  auto cursors = wh::core::cursor_reader<decltype(source_two)>::make_readers(
      std::move(source_two), 2U);
  REQUIRE(cursors.size() == 2U);
  STATIC_REQUIRE(
      !std::copy_constructible<std::remove_cvref_t<decltype(cursors.front())>>);

  auto first_left = cursors[0].read();
  auto first_right = cursors[1].read();
  REQUIRE(first_left.has_value());
  REQUIRE(first_right.has_value());
  REQUIRE(first_left.value().value == std::optional<std::string>{"alpha"});
  REQUIRE(first_right.value().value == std::optional<std::string>{"alpha"});

  auto second_right = cursors[1].read();
  auto second_left = cursors[0].read();
  REQUIRE(second_left.has_value());
  REQUIRE(second_right.has_value());
  REQUIRE(second_left.value().value == std::optional<std::string>{"beta"});
  REQUIRE(second_right.value().value == std::optional<std::string>{"beta"});

  auto third_left = cursors[0].read();
  auto third_right = cursors[1].read();
  REQUIRE(third_left.has_value());
  REQUIRE(third_right.has_value());
  REQUIRE(third_left.value().value == std::optional<std::string>{"gamma"});
  REQUIRE(third_right.value().value == std::optional<std::string>{"gamma"});

  auto eof_left = cursors[0].read();
  auto eof_right = cursors[1].read();
  REQUIRE(eof_left.has_value());
  REQUIRE(eof_right.has_value());
  REQUIRE(eof_left.value().eof);
  REQUIRE(eof_right.value().eof);
}

TEST_CASE("cursor reader async read exposes sender surface and supports stop",
          "[UT][wh/core/cursor_reader/cursor_reader.hpp][cursor_reader::read_async][condition][branch][concurrency]") {
  auto [writer, source] = wh::schema::stream::make_pipe_stream<int>(1U);
  auto cursors = wh::core::make_cursor_readers(std::move(source), 1U);
  REQUIRE(cursors.size() == 1U);

  using cursor_t = decltype(cursors)::value_type;
  using result_t = decltype(cursors.front().read());
  static_assert(
      stdexec::sender<std::remove_cvref_t<decltype(std::declval<const cursor_t &>()
                                                       .read_async())>>);

  cursor_read_receiver_state<result_t> stopped_state{};
  std::stop_source stop_source{};
  auto stopped_operation = stdexec::connect(
      cursors.front().read_async(),
      cursor_read_receiver<result_t>{
          &stopped_state,
          {.scheduler = stdexec::inline_scheduler{},
           .stop_token = stop_source.get_token()}});

  stdexec::start(stopped_operation);
  REQUIRE_FALSE(stopped_state.value_called);
  REQUIRE_FALSE(stopped_state.error_called);
  REQUIRE_FALSE(stopped_state.stopped_called);

  stop_source.request_stop();

  REQUIRE(stopped_state.stopped_called);
  REQUIRE_FALSE(stopped_state.value_called);
  REQUIRE_FALSE(stopped_state.error_called);

  wh::testing::helper::static_thread_scheduler_helper scheduler{1U};
  std::jthread producer([stream_writer = std::move(writer)]() mutable {
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    REQUIRE(stream_writer.try_write(17).has_value());
    REQUIRE(stream_writer.close().has_value());
  });

  auto waited = stdexec::sync_wait(
      stdexec::starts_on(scheduler.scheduler(), cursors.front().read_async()));
  REQUIRE(waited.has_value());

  auto value = std::move(std::get<0>(waited.value()));
  REQUIRE(value.has_value());
  REQUIRE(value.value().value.has_value());
  REQUIRE(*value.value().value == 17);
}

TEST_CASE("cursor reader close reports not found after release",
          "[UT][wh/core/cursor_reader/cursor_reader.hpp][cursor_reader::close][boundary][branch]") {
  auto [writer, source] = wh::schema::stream::make_pipe_stream<int>(1U);
  auto cursors = wh::core::make_cursor_readers(std::move(source), 1U);
  REQUIRE(cursors.size() == 1U);

  REQUIRE(cursors.front().close().has_value());
  REQUIRE(cursors.front().is_closed());
  REQUIRE(cursors.front().close().has_error());
  REQUIRE(cursors.front().close().error() == wh::core::errc::not_found);

  REQUIRE(writer.close().has_value());
}

TEST_CASE("cursor reader destroys retained unread values when all cursors leave scope",
          "[UT][wh/core/cursor_reader/cursor_reader.hpp][make_readers][lifetime][regression]") {
  retained_cursor_probe::live_count.store(0, std::memory_order_release);

  {
    auto [writer, source] =
        wh::schema::stream::make_pipe_stream<retained_cursor_probe>(2U);
    REQUIRE(writer.try_write(retained_cursor_probe{17}).has_value());
    REQUIRE(writer.close().has_value());

    auto cursors = wh::core::make_cursor_readers(std::move(source), 2U);
    REQUIRE(cursors.size() == 2U);

    auto first = cursors[0].read();
    REQUIRE(first.has_value());
    REQUIRE(first.value().value.has_value());
    REQUIRE(first.value().value->value == 17);

    // Leave the sibling cursor unread so the retained slot must be released
    // during shared-state teardown rather than normal reclamation.
    REQUIRE(retained_cursor_probe::live_count.load(std::memory_order_acquire) >= 1);
  }

  REQUIRE(retained_cursor_probe::live_count.load(std::memory_order_acquire) == 0);
}
