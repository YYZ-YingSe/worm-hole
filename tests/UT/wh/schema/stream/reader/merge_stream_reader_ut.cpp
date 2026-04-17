#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include "helper/sender_capture.hpp"
#include "helper/sender_env.hpp"
#include "wh/schema/stream/pipe.hpp"
#include "wh/schema/stream/reader/merge_stream_reader.hpp"

namespace {

struct throwing_scheduler_state {
  std::size_t connect_calls{0U};
  std::optional<std::size_t> fail_on_connect{};
};

struct throwing_scheduler {
  using scheduler_concept = stdexec::scheduler_t;

  template <typename receiver_t> struct schedule_op {
    using operation_state_concept = stdexec::operation_state_t;

    receiver_t receiver;

    auto start() noexcept -> void { stdexec::set_value(std::move(receiver)); }
  };

  struct schedule_sender {
    throwing_scheduler_state *state{nullptr};

    using sender_concept = stdexec::sender_t;
    using completion_signatures =
        stdexec::completion_signatures<stdexec::set_value_t()>;

    template <typename receiver_t>
    auto connect(receiver_t receiver) const -> schedule_op<receiver_t> {
      ++state->connect_calls;
      if (state->fail_on_connect.has_value() &&
          state->connect_calls == *state->fail_on_connect) {
        throw std::runtime_error("schedule connect failed");
      }
      return schedule_op<receiver_t>{std::move(receiver)};
    }

    [[nodiscard]] auto get_env() const noexcept -> stdexec::env<> { return {}; }
  };

  throwing_scheduler_state *state{nullptr};

  [[nodiscard]] auto schedule() const noexcept -> schedule_sender {
    return schedule_sender{state};
  }

  [[nodiscard]] auto operator==(const throwing_scheduler &) const noexcept
      -> bool = default;
};

template <typename value_t, typename env_t> struct single_completion_state {
  int value_count{0};
  int error_count{0};
  int stopped_count{0};
  std::optional<value_t> value{};
};

template <typename value_t, typename env_t> struct single_completion_receiver {
  using receiver_concept = stdexec::receiver_t;

  single_completion_state<value_t, env_t> *state{nullptr};
  env_t env{};

  auto set_value(value_t value) && noexcept -> void {
    ++state->value_count;
    state->value.emplace(std::move(value));
  }

  template <typename error_t> auto set_error(error_t &&) && noexcept -> void {
    ++state->error_count;
  }

  auto set_stopped() && noexcept -> void { ++state->stopped_count; }

  [[nodiscard]] auto get_env() const noexcept -> env_t { return env; }
};

} // namespace

TEST_CASE("merge stream reader detail helpers sort and label lanes deterministically",
          "[UT][wh/schema/stream/reader/merge_stream_reader.hpp][sort_named_stream_readers][branch][boundary]") {
  using reader_t = wh::schema::stream::pipe_stream_reader<int>;
  auto [writer_z, reader_z] = wh::schema::stream::make_pipe_stream<int>(1U);
  auto [writer_a, reader_a] = wh::schema::stream::make_pipe_stream<int>(1U);
  REQUIRE(writer_z.close().has_value());
  REQUIRE(writer_a.close().has_value());

  std::vector<wh::schema::stream::named_stream_reader<reader_t>> readers{};
  readers.emplace_back("z", std::move(reader_z));
  readers.emplace_back("a", std::move(reader_a));
  wh::schema::stream::detail::sort_named_stream_readers(readers);
  REQUIRE(readers[0].source == "a");
  REQUIRE(readers[1].source == "z");

  std::vector<reader_t> unlabeled{};
  auto [writer0, reader0] = wh::schema::stream::make_pipe_stream<int>(1U);
  auto [writer1, reader1] = wh::schema::stream::make_pipe_stream<int>(1U);
  REQUIRE(writer0.close().has_value());
  REQUIRE(writer1.close().has_value());
  unlabeled.push_back(std::move(reader0));
  unlabeled.push_back(std::move(reader1));
  auto named = wh::schema::stream::detail::make_named_stream_readers(std::move(unlabeled));
  REQUIRE(named.size() == 2U);
  REQUIRE(named[0].source == "0");
  REQUIRE(named[1].source == "1");
}

TEST_CASE("merge stream reader covers fixed dynamic attach disable share and source eof branches",
          "[UT][wh/schema/stream/reader/merge_stream_reader.hpp][merge_stream_reader::attach][condition][branch][concurrency]") {
  {
    std::vector<wh::schema::stream::pipe_stream_reader<int>> readers{};
    std::vector<wh::schema::stream::pipe_stream_writer<int>> writers{};
    for (int i = 0; i < 5; ++i) {
      auto [writer, reader] = wh::schema::stream::make_pipe_stream<int>(1U);
      REQUIRE(writer.try_write(i).has_value());
      REQUIRE(writer.close().has_value());
      writers.push_back(std::move(writer));
      readers.push_back(std::move(reader));
    }
    auto fixed = wh::schema::stream::make_merge_stream_reader(std::move(readers));
    REQUIRE(fixed.uses_fixed_poll_path());
  }

  {
    std::vector<wh::schema::stream::pipe_stream_reader<int>> readers{};
    std::vector<wh::schema::stream::pipe_stream_writer<int>> writers{};
    for (int i = 0; i < 6; ++i) {
      auto [writer, reader] = wh::schema::stream::make_pipe_stream<int>(1U);
      REQUIRE(writer.try_write(i).has_value());
      REQUIRE(writer.close().has_value());
      writers.push_back(std::move(writer));
      readers.push_back(std::move(reader));
    }
    auto dynamic = wh::schema::stream::make_merge_stream_reader(std::move(readers));
    REQUIRE_FALSE(dynamic.uses_fixed_poll_path());
  }

  auto [writer_a, reader_a] = wh::schema::stream::make_pipe_stream<int>(2U);
  auto [writer_b, reader_b] = wh::schema::stream::make_pipe_stream<int>(2U);
  REQUIRE(writer_a.try_write(1).has_value());
  REQUIRE(writer_b.try_write(2).has_value());
  REQUIRE(writer_a.close().has_value());
  REQUIRE(writer_b.close().has_value());

  using reader_t = wh::schema::stream::pipe_stream_reader<int>;
  std::vector<wh::schema::stream::named_stream_reader<reader_t>> named{};
  named.emplace_back("B", std::move(reader_b));
  named.emplace_back("A", std::move(reader_a));
  auto merged = wh::schema::stream::make_merge_stream_reader(std::move(named));

  auto first = merged.try_read();
  REQUIRE(std::holds_alternative<wh::schema::stream::stream_result<
              wh::schema::stream::stream_chunk<int>>>(first));
  auto first_value = std::move(std::get<wh::schema::stream::stream_result<
      wh::schema::stream::stream_chunk<int>>>(first));
  REQUIRE(first_value.has_value());
  REQUIRE(first_value.value().value.has_value());
  REQUIRE((first_value.value().source == "B" ||
           first_value.value().source == "A"));

  auto shared = merged.share();
  REQUIRE(shared.close().has_value());
  REQUIRE(merged.is_closed());
}

TEST_CASE("merge stream reader async stop restart and pending attach paths remain usable",
          "[UT][wh/schema/stream/reader/merge_stream_reader.hpp][merge_stream_reader::read_async][concurrency][branch]") {
  using reader_t = wh::schema::stream::pipe_stream_reader<int>;
  auto merged = wh::schema::stream::make_merge_stream_reader<reader_t>(
      std::vector<std::string>{"A"});

  using result_t =
      wh::schema::stream::stream_result<wh::schema::stream::stream_chunk<int>>;
  using env_t = wh::testing::helper::scheduler_env<
      stdexec::inline_scheduler, stdexec::inplace_stop_token>;

  wh::testing::helper::sender_capture<result_t> capture{};
  stdexec::inplace_stop_source stop_source{};
  auto operation = stdexec::connect(
      merged.read_async(),
      wh::testing::helper::sender_capture_receiver<result_t, env_t>{
          &capture, {.scheduler = stdexec::inline_scheduler{},
                     .stop_token = stop_source.get_token()}});
  stdexec::start(operation);
  REQUIRE_FALSE(capture.ready.try_acquire());
  stop_source.request_stop();
  REQUIRE(capture.ready.try_acquire());
  REQUIRE(capture.terminal == wh::testing::helper::sender_terminal_kind::stopped);

  auto [writer, reader] = wh::schema::stream::make_pipe_stream<int>(2U);
  REQUIRE(writer.try_write(33).has_value());
  REQUIRE(writer.close().has_value());
  REQUIRE(merged.attach("A", std::move(reader)).has_value());

  auto waited = stdexec::sync_wait(merged.read_async());
  REQUIRE(waited.has_value());
  auto next = std::move(std::get<0>(waited.value()));
  REQUIRE(next.has_value());
  REQUIRE(next.value().value == std::optional<int>{33});
  REQUIRE(next.value().source == "A");

  auto live = wh::schema::stream::make_merge_stream_reader<reader_t>(
      std::vector<std::string>{"A", "B"});
  REQUIRE(live.disable("B").has_value());
  auto [writer_a, reader_attached] = wh::schema::stream::make_pipe_stream<int>(2U);
  REQUIRE(live.attach("A", std::move(reader_attached)).has_value());
  REQUIRE(writer_a.try_write(9).has_value());
  REQUIRE(writer_a.close().has_value());

  auto value = live.read();
  REQUIRE(value.has_value());
  REQUIRE(value.value().value == std::optional<int>{9});
  REQUIRE(value.value().source == "A");
  auto source_eof = live.read();
  REQUIRE(source_eof.has_value());
  REQUIRE(source_eof.value().eof);
  REQUIRE(source_eof.value().source == "A");
  auto terminal = live.read();
  REQUIRE(terminal.has_value());
  REQUIRE(terminal.value().eof);
  REQUIRE(terminal.value().source.empty());
}

TEST_CASE("merge stream reader resume scheduling failure delivers one terminal failure",
          "[UT][wh/schema/stream/reader/merge_stream_reader.hpp][merge_stream_reader::read_async][resume][failure]") {
  using reader_t = wh::schema::stream::pipe_stream_reader<int>;
  using result_t =
      wh::schema::stream::stream_result<wh::schema::stream::stream_chunk<int>>;

  auto [writer, reader] = wh::schema::stream::make_pipe_stream<int>(2U);
  REQUIRE(writer.try_write(7).has_value());
  REQUIRE(writer.close().has_value());

  std::vector<reader_t> readers{};
  readers.push_back(std::move(reader));
  auto merged = wh::schema::stream::make_merge_stream_reader(std::move(readers));

  throwing_scheduler_state scheduler_state{};
  scheduler_state.fail_on_connect = 1U;
  auto env =
      wh::testing::helper::make_scheduler_env(throwing_scheduler{&scheduler_state});

  using env_t = decltype(env);
  single_completion_state<result_t, env_t> state{};
  auto operation = stdexec::connect(
      merged.read_async(),
      single_completion_receiver<result_t, env_t>{&state, env});
  stdexec::start(operation);

  REQUIRE(scheduler_state.connect_calls == 1U);
  REQUIRE(state.value_count == 1);
  REQUIRE(state.error_count == 0);
  REQUIRE(state.stopped_count == 0);
  REQUIRE(state.value.has_value());
  REQUIRE(state.value->has_error());
  REQUIRE(state.value->error() == wh::core::errc::internal_error);
}
