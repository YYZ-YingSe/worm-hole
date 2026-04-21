#include <chrono>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include "helper/component_contract_support.hpp"
#include "helper/compose_graph_test_utils.hpp"
#include "helper/sender_capture.hpp"
#include "helper/sender_env.hpp"
#include "helper/static_thread_scheduler.hpp"
#include "wh/compose/graph/stream.hpp"
#include "wh/compose/node/detail/tools/output.hpp"
#include "wh/compose/node/detail/tools/tool_event_stream_reader.hpp"
#include "wh/core/type_utils.hpp"

namespace {

using wh::testing::helper::collect_int_graph_chunk_values;
using wh::testing::helper::collect_int_graph_stream;
using wh::testing::helper::collect_string_graph_chunk_values;
using wh::testing::helper::invoke_graph_sync;
using wh::testing::helper::invoke_value_sync;
using wh::testing::helper::make_auto_contract_edge_options;
using wh::testing::helper::make_dual_scheduler_env;
using wh::testing::helper::make_graph_request;
using wh::testing::helper::make_int_graph_stream;
using wh::testing::helper::read_any;
using wh::testing::helper::read_graph_value;
using wh::testing::helper::sum_ints;

template <typename value_t>
auto require_value_capture(wh::testing::helper::sender_capture<value_t> &capture,
                           const std::chrono::milliseconds timeout = std::chrono::milliseconds{500})
    -> value_t {
  REQUIRE(capture.ready.try_acquire_for(timeout));
  REQUIRE(capture.terminal == wh::testing::helper::sender_terminal_kind::value);
  REQUIRE(capture.value.has_value());
  return std::move(*capture.value);
}

[[nodiscard]] auto make_int_stream(std::initializer_list<int> values)
    -> wh::compose::graph_stream_reader {
  auto [writer, reader] = wh::compose::make_graph_stream(values.size() + 1U);
  for (const auto value : values) {
    REQUIRE(writer.try_write(wh::core::any(value)).has_value());
  }
  REQUIRE(writer.close().has_value());
  return std::move(reader);
}

[[nodiscard]] auto collect_ints(wh::compose::graph_stream_reader reader)
    -> wh::core::result<std::vector<int>> {
  auto collected = wh::compose::collect_graph_stream_reader(std::move(reader));
  if (collected.has_error()) {
    return wh::core::result<std::vector<int>>::failure(collected.error());
  }

  std::vector<int> values{};
  values.reserve(collected.value().size());
  for (auto &payload : collected.value()) {
    const auto *typed = wh::core::any_cast<int>(&payload);
    if (typed == nullptr) {
      return wh::core::result<std::vector<int>>::failure(wh::core::errc::type_mismatch);
    }
    values.push_back(*typed);
  }
  return values;
}

[[nodiscard]] auto read_int_payload(wh::compose::graph_value &&payload) -> wh::core::result<int> {
  if (auto *typed = wh::core::any_cast<int>(&payload); typed != nullptr) {
    return std::move(*typed);
  }
  return wh::core::result<int>::failure(wh::core::errc::type_mismatch);
}

[[nodiscard]] auto read_string_payload(wh::compose::graph_value &payload)
    -> wh::core::result<std::string> {
  if (auto *typed = wh::core::any_cast<std::string>(&payload); typed != nullptr) {
    return *typed;
  }
  return wh::core::result<std::string>::failure(wh::core::errc::type_mismatch);
}

struct inline_async_int_reader {
  using value_type = int;
  using chunk_type = wh::schema::stream::stream_chunk<int>;
  using result_type = wh::schema::stream::stream_result<chunk_type>;

  int value{0};
  bool closed{false};

  [[nodiscard]] auto read() -> result_type {
    if (closed) {
      return chunk_type::make_eof();
    }
    closed = true;
    return chunk_type::make_value(value);
  }

  [[nodiscard]] auto try_read() -> wh::schema::stream::stream_try_result<chunk_type> {
    return read();
  }

  auto close() -> wh::core::result<void> {
    closed = true;
    return {};
  }

  [[nodiscard]] auto is_closed() const noexcept -> bool { return closed; }

  [[nodiscard]] auto read_async() & {
    if (closed) {
      return stdexec::just(result_type{chunk_type::make_eof()});
    }
    closed = true;
    return stdexec::just(result_type{chunk_type::make_value(value)});
  }
};

struct handoff_lifetime_probe {
  bool destroyed{false};
  bool destroyed_before_start_return{false};
};

struct handoff_probe_scheduler {
  struct schedule_sender {
    using sender_concept = stdexec::sender_t;
    using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t()>;

    std::shared_ptr<handoff_lifetime_probe> probe{};

    template <stdexec::receiver_of<completion_signatures> receiver_t> struct operation {
      using operation_state_concept = stdexec::operation_state_t;

      receiver_t receiver;
      std::shared_ptr<handoff_lifetime_probe> probe{};

      auto start() & noexcept -> void {
        auto keep_alive = probe;
        stdexec::set_value(std::move(receiver));
        if (keep_alive->destroyed) {
          keep_alive->destroyed_before_start_return = true;
        }
      }

      ~operation() { probe->destroyed = true; }
    };

    template <stdexec::receiver_of<completion_signatures> receiver_t>
    auto connect(receiver_t receiver) const -> operation<receiver_t> {
      return operation<receiver_t>{
          .receiver = std::move(receiver),
          .probe = probe,
      };
    }

    template <typename self_t, typename... env_t>
      requires std::same_as<std::remove_cvref_t<self_t>, schedule_sender> &&
               (sizeof...(env_t) >= 1U)
    static consteval auto get_completion_signatures() {
      return completion_signatures{};
    }
  };

  std::shared_ptr<handoff_lifetime_probe> probe{};

  [[nodiscard]] auto schedule() const noexcept -> schedule_sender { return schedule_sender{probe}; }

  [[nodiscard]] auto operator==(const handoff_probe_scheduler &) const noexcept -> bool = default;
};

} // namespace

static_assert(!std::copy_constructible<wh::compose::graph_stream_reader>);
static_assert(!std::is_copy_assignable_v<wh::compose::graph_stream_reader>);
static_assert(std::movable<wh::compose::graph_stream_reader>);

TEST_CASE("graph reader explicit copy preserves fanout semantics",
          "[core][compose][graph_stream][fanout]") {
  auto source = make_int_stream({1, 2, 3, 4});
  auto copies = wh::compose::detail::copy_graph_readers(std::move(source), 2U);
  REQUIRE(copies.has_value());
  REQUIRE(copies.value().size() == 2U);

  auto left = collect_ints(std::move(copies.value()[0]));
  auto right = collect_ints(std::move(copies.value()[1]));
  REQUIRE(left.has_value());
  REQUIRE(right.has_value());
  REQUIRE(left.value() == std::vector<int>{1, 2, 3, 4});
  REQUIRE(right.value() == std::vector<int>{1, 2, 3, 4});
}

TEST_CASE("graph single-value stream preserves nested reader payload",
          "[core][compose][graph_stream][payload]") {
  auto reader = make_int_stream({4, 5});
  wh::compose::graph_value payload = wh::core::any(std::move(reader));

  auto wrapped = wh::compose::make_single_value_stream_reader(std::move(payload));
  REQUIRE(wrapped.has_value());

  auto collected = wh::compose::collect_graph_stream_reader(std::move(wrapped).value());
  REQUIRE(collected.has_value());
  REQUIRE(collected.value().size() == 1U);

  auto *nested_reader =
      wh::core::any_cast<wh::compose::graph_stream_reader>(&collected.value().front());
  REQUIRE(nested_reader != nullptr);

  auto nested_values = collect_ints(std::move(*nested_reader));
  REQUIRE(nested_values.has_value());
  REQUIRE(nested_values.value() == std::vector<int>{4, 5});
}

TEST_CASE("graph value fork rejects embedded reader payload",
          "[core][compose][graph_stream][payload]") {
  auto reader = make_int_stream({7, 8});
  wh::compose::graph_value payload = wh::core::any(std::move(reader));

  auto forked = wh::compose::fork_graph_value(payload);
  REQUIRE(forked.has_error());
  REQUIRE(forked.error() == wh::core::errc::contract_violation);
}

TEST_CASE("graph stream explicit copies preserve fanout semantics",
          "[core][compose][graph_stream][concurrency]") {
  auto [writer, source] = wh::schema::stream::make_pipe_stream<wh::compose::graph_value>(9U);
  for (int value = 1; value <= 8; ++value) {
    REQUIRE(writer.try_write(wh::core::any(value)).has_value());
  }
  REQUIRE(writer.close().has_value());

  auto copies = wh::schema::stream::make_copy_stream_readers(std::move(source), 2U);
  REQUIRE(copies.size() == 2U);

  wh::compose::graph_stream_reader left_reader{std::move(copies[0])};
  wh::compose::graph_stream_reader right_reader{std::move(copies[1])};

  wh::core::result<std::vector<int>> left{};
  wh::core::result<std::vector<int>> right{};

  wh::testing::helper::joining_thread left_thread(
      [&]() { left = collect_ints(std::move(left_reader)); });
  wh::testing::helper::joining_thread right_thread(
      [&]() { right = collect_ints(std::move(right_reader)); });

  left_thread.join();
  right_thread.join();

  REQUIRE(left.has_value());
  REQUIRE(right.has_value());
  REQUIRE(left.value() == std::vector<int>{1, 2, 3, 4, 5, 6, 7, 8});
  REQUIRE(right.value() == std::vector<int>{1, 2, 3, 4, 5, 6, 7, 8});
}

TEST_CASE("graph stream async merge forwards merged lane reads",
          "[core][compose][graph_stream][async]") {
  auto [left_writer, left_reader] = wh::compose::make_graph_stream(2U);
  auto [right_writer, right_reader] = wh::compose::make_graph_stream(2U);

  std::vector<wh::schema::stream::named_stream_reader<wh::compose::graph_stream_reader>> lanes{};
  lanes.push_back({"left", std::move(left_reader), false});
  lanes.push_back({"right", std::move(right_reader), false});
  auto merged = wh::compose::detail::make_graph_merge_reader(std::move(lanes));

  wh::testing::helper::static_thread_scheduler_helper scheduler{1U};
  wh::testing::helper::joining_thread producer(
      [writer = std::move(right_writer), left = std::move(left_writer)]() mutable {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        [[maybe_unused]] const auto write_status = writer.try_write(wh::core::any(19));
        [[maybe_unused]] const auto close_status = writer.close();
        [[maybe_unused]] const auto left_close = left.close();
      });

  auto waited =
      stdexec::sync_wait(stdexec::starts_on(scheduler.scheduler(), std::move(merged).read_async()));
  REQUIRE(waited.has_value());

  auto first = std::move(std::get<0>(waited.value()));
  REQUIRE(first.has_value());
  REQUIRE(first.value().value.has_value());
  REQUIRE(wh::core::any_cast<int>(*first.value().value) == 19);
  REQUIRE(first.value().source == "right");
}

TEST_CASE("graph stream async merge remains stable across repeated inline lane completions",
          "[core][compose][graph_stream][async]") {
  wh::testing::helper::static_thread_scheduler_helper scheduler{1U};

  for (int iteration = 0; iteration < 512; ++iteration) {
    using reader_t = inline_async_int_reader;
    std::vector<wh::schema::stream::named_stream_reader<reader_t>> lanes{};
    lanes.push_back({"lane", reader_t{.value = iteration}, false});

    auto merged = wh::schema::stream::make_merge_stream_reader(std::move(lanes));
    auto waited = stdexec::sync_wait(
        stdexec::starts_on(scheduler.scheduler(), std::move(merged).read_async()));
    REQUIRE(waited.has_value());

    auto next = std::move(std::get<0>(waited.value()));
    REQUIRE(next.has_value());
    REQUIRE(!next.value().eof);
    REQUIRE(next.value().value.has_value());
    REQUIRE(*next.value().value == iteration);
    REQUIRE(next.value().source == "lane");
  }
}

TEST_CASE(
    "graph stream async merge handoff does not destroy scheduler entry before callback returns",
    "[core][compose][graph_stream][async][lifetime]") {
  using reader_t = inline_async_int_reader;
  using merge_t = wh::schema::stream::merge_stream_reader<reader_t>;
  using status_t = typename merge_t::status_type;

  auto probe = std::make_shared<handoff_lifetime_probe>();
  std::vector<wh::schema::stream::named_stream_reader<reader_t>> lanes{};
  lanes.push_back({"lane", reader_t{.value = 19}, false});

  auto merged = wh::schema::stream::make_merge_stream_reader<reader_t>(std::move(lanes));

  status_t status{};
  REQUIRE(wh::testing::helper::wait_for_value(
      merged.read_async(), status, std::chrono::milliseconds{500},
      wh::testing::helper::make_scheduler_env(handoff_probe_scheduler{probe})));

  REQUIRE(status.has_value());
  REQUIRE(status.value().value.has_value());
  REQUIRE(*status.value().value == 19);
  REQUIRE(status.value().source == "lane");
  REQUIRE(probe->destroyed);
  REQUIRE_FALSE(probe->destroyed_before_start_return);
}

TEST_CASE("graph stream async merge remains stable across repeated scheduler handoffs",
          "[core][compose][graph_stream][async][stress]") {
  using reader_t = inline_async_int_reader;
  using merge_t = wh::schema::stream::merge_stream_reader<reader_t>;
  using status_t = typename merge_t::status_type;

  for (int iteration = 0; iteration < 512; ++iteration) {
    auto probe = std::make_shared<handoff_lifetime_probe>();
    std::vector<wh::schema::stream::named_stream_reader<reader_t>> lanes{};
    lanes.push_back({"lane", reader_t{.value = iteration}, false});

    auto merged = wh::schema::stream::make_merge_stream_reader<reader_t>(std::move(lanes));

    status_t status{};
    REQUIRE(wh::testing::helper::wait_for_value(
        merged.read_async(), status, std::chrono::milliseconds{500},
        wh::testing::helper::make_scheduler_env(handoff_probe_scheduler{probe})));
    REQUIRE(status.has_value());
    REQUIRE(status.value().value.has_value());
    REQUIRE(*status.value().value == iteration);
    REQUIRE(status.value().source == "lane");
    REQUIRE_FALSE(probe->destroyed_before_start_return);
  }
}

TEST_CASE("graph stream blocking merge wakes when pending lane is disabled",
          "[core][compose][graph_stream][topology][blocking]") {
  auto merged = wh::schema::stream::make_merge_stream_reader<wh::compose::graph_stream_reader>(
      std::vector<std::string>{"late"});

  std::optional<wh::compose::graph_stream_reader::chunk_result_type> result{};
  wh::testing::helper::joining_thread consumer([&] { result.emplace(merged.read()); });

  std::this_thread::sleep_for(std::chrono::milliseconds{10});
  REQUIRE(merged.disable("late").has_value());

  consumer.join();
  REQUIRE(result.has_value());
  REQUIRE(result->has_value());
  REQUIRE(result->value().eof);
}

TEST_CASE("graph stream async merge pending read propagates outer stop",
          "[core][compose][graph_stream][topology][stop]") {
  auto merged = wh::schema::stream::make_merge_stream_reader<wh::compose::graph_stream_reader>(
      std::vector<std::string>{"late"});

  wh::testing::helper::static_thread_scheduler_helper scheduler{1U};
  stdexec::inplace_stop_source stop_source{};
  wh::testing::helper::sender_capture<> completion{};

  auto operation = stdexec::connect(stdexec::starts_on(scheduler.scheduler(), merged.read_async()),
                                    wh::testing::helper::sender_capture_receiver{
                                        &completion,
                                        scheduler.env(stop_source.get_token()),
                                    });
  stdexec::start(operation);

  stop_source.request_stop();

  REQUIRE(completion.ready.try_acquire_for(std::chrono::milliseconds{500}));
  REQUIRE(completion.terminal == wh::testing::helper::sender_terminal_kind::stopped);
}

TEST_CASE("graph stream async merge pending read resumes when lane attaches",
          "[core][compose][graph_stream][topology][async]") {
  auto merged = wh::schema::stream::make_merge_stream_reader<wh::compose::graph_stream_reader>(
      std::vector<std::string>{"late"});

  wh::testing::helper::static_thread_scheduler_helper scheduler{1U};
  wh::compose::graph_stream_reader::chunk_result_type value{};
  auto sender = stdexec::starts_on(scheduler.scheduler(), merged.read_async());
  std::optional<wh::core::result<void>> attach_status{};
  std::optional<wh::core::result<void>> write_status{};
  std::optional<wh::core::result<void>> close_status{};

  wh::testing::helper::joining_thread producer([&] {
    try {
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
      auto [writer, reader] = wh::compose::make_graph_stream(2U);
      attach_status.emplace(merged.attach("late", std::move(reader)));
      if (!attach_status->has_value()) {
        return;
      }
      write_status.emplace(writer.try_write(wh::core::any(31)));
      if (!write_status->has_value()) {
        return;
      }
      close_status.emplace(writer.close());
    } catch (...) {
      if (!attach_status.has_value()) {
        attach_status.emplace(wh::core::result<void>::failure(wh::core::errc::internal_error));
      }
      if (!write_status.has_value()) {
        write_status.emplace(wh::core::result<void>::failure(wh::core::errc::internal_error));
      }
      if (!close_status.has_value()) {
        close_status.emplace(wh::core::result<void>::failure(wh::core::errc::internal_error));
      }
    }
  });

  REQUIRE(wh::testing::helper::wait_for_value(sender, value, std::chrono::milliseconds{500},
                                              scheduler.env()));
  producer.join();

  REQUIRE(attach_status.has_value());
  REQUIRE(attach_status->has_value());
  REQUIRE(write_status.has_value());
  REQUIRE(write_status->has_value());
  REQUIRE(close_status.has_value());
  REQUIRE(close_status->has_value());
  REQUIRE(value.has_value());
  REQUIRE_FALSE(value.value().eof);
  REQUIRE(value.value().value.has_value());
  REQUIRE(wh::core::any_cast<int>(*value.value().value) == 31);
  REQUIRE(value.value().source == "late");
}

TEST_CASE("tools event stream async read preserves stopped and remains readable after resume",
          "[core][compose][graph_stream][tools][stop]") {
  auto [writer, source] = wh::compose::make_graph_stream(4U);
  wh::compose::tools_options options{};
  wh::compose::detail::tools_state state{};
  state.options = &options;
  std::vector<wh::compose::detail::stream_completion> stream_inputs{};
  stream_inputs.push_back(wh::compose::detail::stream_completion{.index = 0U,
                                                                 .call =
                                                                     wh::compose::tool_call{
                                                                         .call_id = "call-1",
                                                                         .tool_name = "echo",
                                                                         .arguments = "payload",
                                                                     },
                                                                 .stream = std::move(source),
                                                                 .rerun_extra = {}});
  auto output = wh::compose::detail::build_stream_output(state, std::move(stream_inputs));
  REQUIRE(output.has_value());
  auto *wrapped = wh::core::any_cast<wh::compose::graph_stream_reader>(&output.value());
  REQUIRE(wrapped != nullptr);

  wh::testing::helper::sender_capture<> completion{};
  stdexec::inplace_stop_source stop_source{};
  auto operation = stdexec::connect(wrapped->read_async(),
                                    wh::testing::helper::sender_capture_receiver{
                                        &completion,
                                        wh::testing::helper::make_scheduler_env(
                                            stdexec::inline_scheduler{}, stop_source.get_token()),
                                    });

  stdexec::start(operation);
  stop_source.request_stop();

  REQUIRE(completion.ready.try_acquire_for(std::chrono::milliseconds(100)));
  REQUIRE(completion.terminal == wh::testing::helper::sender_terminal_kind::stopped);

  REQUIRE(writer.try_write(wh::core::any(std::string{"chunk-1"})).has_value());
  REQUIRE(writer.close().has_value());

  auto resumed = wrapped->read();
  REQUIRE(resumed.has_value());
  REQUIRE_FALSE(resumed.value().eof);
  REQUIRE_FALSE(resumed.value().error.failed());
  REQUIRE(resumed.value().value.has_value());

  auto *event = wh::core::any_cast<wh::compose::tool_event>(&*resumed.value().value);
  REQUIRE(event != nullptr);
  REQUIRE(event->call_id == "call-1");
  REQUIRE(event->tool_name == "echo");
  auto text = read_string_payload(event->value);
  REQUIRE(text.has_value());
  REQUIRE(text.value() == "chunk-1");

  auto eof = wrapped->read();
  REQUIRE(eof.has_value());
  REQUIRE(eof.value().eof);
}

TEST_CASE("graph stream fanout into two live merges keeps blocking readers isolated",
          "[core][compose][graph_stream][merge][fanout]") {
  auto fast_copies = wh::schema::stream::make_copy_stream_readers(make_int_stream({1, 2}), 2U);
  auto slow_copies = wh::schema::stream::make_copy_stream_readers(make_int_stream({10, 11}), 2U);
  REQUIRE(fast_copies.size() == 2U);
  REQUIRE(slow_copies.size() == 2U);

  using reader_t = wh::compose::graph_stream_reader;
  auto left_merge = std::make_shared<wh::schema::stream::merge_stream_reader<reader_t>>(
      wh::schema::stream::make_merge_stream_reader<reader_t>(
          std::vector<std::string>{"fast", "slow"}));
  auto right_merge = std::make_shared<wh::schema::stream::merge_stream_reader<reader_t>>(
      wh::schema::stream::make_merge_stream_reader<reader_t>(
          std::vector<std::string>{"fast", "slow"}));

  REQUIRE(left_merge->attach("fast", reader_t{std::move(fast_copies[0])}).has_value());
  REQUIRE(right_merge->attach("fast", reader_t{std::move(fast_copies[1])}).has_value());

  auto drain = [](reader_t reader) -> wh::core::result<std::vector<int>> {
    std::vector<int> values{};
    std::unordered_set<std::string> sources{};
    for (;;) {
      auto next = reader.read();
      if (next.has_error()) {
        return wh::core::result<std::vector<int>>::failure(next.error());
      }
      auto chunk = std::move(next).value();
      if (chunk.error != wh::core::errc::ok) {
        return wh::core::result<std::vector<int>>::failure(chunk.error);
      }
      if (chunk.is_terminal_eof()) {
        break;
      }
      if (chunk.is_source_eof()) {
        continue;
      }
      if (!chunk.value.has_value()) {
        return wh::core::result<std::vector<int>>::failure(wh::core::errc::not_found);
      }
      auto typed = read_int_payload(std::move(*chunk.value));
      if (typed.has_error()) {
        return wh::core::result<std::vector<int>>::failure(typed.error());
      }
      values.push_back(typed.value());
      if (!chunk.source.empty()) {
        sources.insert(chunk.source);
      }
    }
    if (sources != std::unordered_set<std::string>{"fast", "slow"}) {
      return wh::core::result<std::vector<int>>::failure(wh::core::errc::type_mismatch);
    }
    return values;
  };

  wh::core::result<std::vector<int>> left_values{};
  wh::core::result<std::vector<int>> right_values{};
  std::optional<wh::core::result<void>> left_attach_status{};
  std::optional<wh::core::result<void>> right_attach_status{};

  wh::testing::helper::joining_thread producer([left_merge, right_merge, &left_attach_status,
                                                &right_attach_status,
                                                slow_left = reader_t{std::move(slow_copies[0])},
                                                slow_right =
                                                    reader_t{std::move(slow_copies[1])}]() mutable {
    try {
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
      left_attach_status.emplace(left_merge->attach("slow", std::move(slow_left)));
      right_attach_status.emplace(right_merge->attach("slow", std::move(slow_right)));
    } catch (...) {
      left_attach_status.emplace(wh::core::result<void>::failure(wh::core::errc::internal_error));
      right_attach_status.emplace(wh::core::result<void>::failure(wh::core::errc::internal_error));
    }
  });

  wh::testing::helper::joining_thread left_thread([&]() {
    try {
      left_values = drain(reader_t{left_merge->share()});
    } catch (...) {
      left_values = wh::core::result<std::vector<int>>::failure(wh::core::errc::internal_error);
    }
  });
  wh::testing::helper::joining_thread right_thread([&]() {
    try {
      right_values = drain(reader_t{right_merge->share()});
    } catch (...) {
      right_values = wh::core::result<std::vector<int>>::failure(wh::core::errc::internal_error);
    }
  });

  left_thread.join();
  right_thread.join();
  producer.join();

  REQUIRE(left_attach_status.has_value());
  REQUIRE(right_attach_status.has_value());
  REQUIRE(left_attach_status->has_value());
  REQUIRE(right_attach_status->has_value());
  const auto left_message =
      left_values.has_error() ? left_values.error().message() : std::string{"left_ok"};
  const auto right_message =
      right_values.has_error() ? right_values.error().message() : std::string{"right_ok"};
  const auto left_code = left_values.has_error() ? left_values.error().value() : 0;
  const auto right_code = right_values.has_error() ? right_values.error().value() : 0;
  INFO(left_message);
  INFO(right_message);
  INFO(left_code);
  INFO(right_code);
  REQUIRE(left_values.has_value());
  REQUIRE(right_values.has_value());
  REQUIRE(left_values.value() == std::vector<int>{1, 2, 10, 11});
  REQUIRE(right_values.value() == std::vector<int>{1, 2, 10, 11});
}

TEST_CASE("compose graph fan-in stream-to-value normalization builds value map",
          "[core][compose][graph][branch]") {
  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::dag;
  options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
  options.fan_in_policy = wh::compose::graph_fan_in_policy::require_all_sources;
  wh::compose::graph graph{std::move(options)};

  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream>(
                  "a",
                  [](const wh::compose::graph_value &, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_stream_reader> {
                    return wh::compose::make_single_value_stream_reader(std::string{"A"});
                  })
              .has_value());
  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream>(
                  "b",
                  [](const wh::compose::graph_value &, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_stream_reader> {
                    return wh::compose::make_single_value_stream_reader(std::string{"B"});
                  })
              .has_value());
  REQUIRE(graph
              .add_lambda(
                  "join",
                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    auto merged = read_any<wh::compose::graph_value_map>(input);
                    if (merged.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(merged.error());
                    }
                    auto left = collect_string_graph_chunk_values(merged.value().at("a"));
                    auto right = collect_string_graph_chunk_values(merged.value().at("b"));
                    if (left.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(left.error());
                    }
                    if (right.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(right.error());
                    }
                    if (left.value().size() != 1U || right.value().size() != 1U) {
                      return wh::core::result<wh::compose::graph_value>::failure(
                          wh::core::errc::contract_violation);
                    }
                    return wh::core::any(left.value().front() + "+" + right.value().front());
                  })
              .has_value());
  REQUIRE(graph.add_entry_edge("a").has_value());
  REQUIRE(graph.add_entry_edge("b").has_value());
  REQUIRE(graph.add_edge("a", "join", make_auto_contract_edge_options()).has_value());
  REQUIRE(graph.add_edge("b", "join", make_auto_contract_edge_options()).has_value());
  REQUIRE(graph.add_exit_edge("join").has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context context{};
  auto invoked = invoke_value_sync(graph, wh::core::any(0), context);
  REQUIRE(invoked.has_value());
  auto typed = read_any<std::string>(invoked.value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == "A+B");
}

TEST_CASE("compose graph fan-in stream-to-value normalization preserves chunk lists",
          "[core][compose][graph][boundary]") {
  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::dag;
  options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
  options.fan_in_policy = wh::compose::graph_fan_in_policy::require_all_sources;
  wh::compose::graph graph{std::move(options)};

  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream>(
                  "a",
                  [](const wh::compose::graph_value &, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_stream_reader> {
                    auto [writer, reader] = wh::compose::make_graph_stream(4U);
                    auto push_1 = writer.try_write(wh::core::any(1));
                    if (push_1.has_error()) {
                      return wh::core::result<wh::compose::graph_stream_reader>::failure(
                          push_1.error());
                    }
                    auto push_2 = writer.try_write(wh::core::any(2));
                    if (push_2.has_error()) {
                      return wh::core::result<wh::compose::graph_stream_reader>::failure(
                          push_2.error());
                    }
                    auto closed = writer.close();
                    if (closed.has_error()) {
                      return wh::core::result<wh::compose::graph_stream_reader>::failure(
                          closed.error());
                    }
                    return std::move(reader);
                  })
              .has_value());
  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream>(
                  "b",
                  [](const wh::compose::graph_value &, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_stream_reader> {
                    auto [writer, reader] = wh::compose::make_graph_stream(4U);
                    auto push_1 = writer.try_write(wh::core::any(3));
                    if (push_1.has_error()) {
                      return wh::core::result<wh::compose::graph_stream_reader>::failure(
                          push_1.error());
                    }
                    auto push_2 = writer.try_write(wh::core::any(4));
                    if (push_2.has_error()) {
                      return wh::core::result<wh::compose::graph_stream_reader>::failure(
                          push_2.error());
                    }
                    auto closed = writer.close();
                    if (closed.has_error()) {
                      return wh::core::result<wh::compose::graph_stream_reader>::failure(
                          closed.error());
                    }
                    return std::move(reader);
                  })
              .has_value());
  REQUIRE(graph
              .add_lambda(
                  "join",
                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    auto merged = read_any<wh::compose::graph_value_map>(input);
                    if (merged.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(merged.error());
                    }
                    auto left = collect_int_graph_chunk_values(merged.value().at("a"));
                    auto right = collect_int_graph_chunk_values(merged.value().at("b"));
                    if (left.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(left.error());
                    }
                    if (right.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(right.error());
                    }
                    return wh::core::any(sum_ints(left.value()) + sum_ints(right.value()));
                  })
              .has_value());
  REQUIRE(graph.add_entry_edge("a").has_value());
  REQUIRE(graph.add_entry_edge("b").has_value());
  REQUIRE(graph.add_edge("a", "join", make_auto_contract_edge_options()).has_value());
  REQUIRE(graph.add_edge("b", "join", make_auto_contract_edge_options()).has_value());
  REQUIRE(graph.add_exit_edge("join").has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context context{};
  auto invoked = invoke_value_sync(graph, wh::core::any(0), context);
  REQUIRE(invoked.has_value());
  auto typed = read_any<int>(invoked.value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == 10);
}

TEST_CASE("compose graph allow-no-data stream input falls back to closed reader",
          "[core][compose][graph][boundary]") {
  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::dag;
  wh::compose::graph graph{std::move(options)};

  wh::compose::graph_add_node_options node_options{};
  node_options.allow_no_control = true;
  node_options.allow_no_data = true;
  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::stream, wh::compose::node_contract::value>(
                  "sink",
                  [](wh::compose::graph_stream_reader input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    if (!input.is_source_closed()) {
                      return wh::core::result<wh::compose::graph_value>::failure(
                          wh::core::errc::contract_violation);
                    }
                    auto values = collect_int_graph_stream(std::move(input));
                    if (values.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(values.error());
                    }
                    return wh::core::any(static_cast<int>(values.value().size()));
                  },
                  std::move(node_options))
              .has_value());
  auto entry_options = make_auto_contract_edge_options();
  entry_options.no_data = true;
  REQUIRE(graph.add_entry_edge("sink", std::move(entry_options)).has_value());
  REQUIRE(graph.add_exit_edge("sink").has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context context{};
  auto invoked = invoke_value_sync(graph, wh::core::any(42), context);
  REQUIRE(invoked.has_value());
  auto typed = read_any<int>(invoked.value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == 0);
}

TEST_CASE("compose graph copyable stream one-to-many fanout remains stable",
          "[core][compose][graph][stream][stress]") {
  wh::compose::graph_compile_options options{};
  options.mode = wh::compose::graph_runtime_mode::dag;
  options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
  options.fan_in_policy = wh::compose::graph_fan_in_policy::require_all_sources;
  wh::compose::graph graph{std::move(options)};
  REQUIRE(
      graph
          .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream>(
              "source",
              [](const wh::compose::graph_value &input, wh::core::run_context &,
                 const wh::compose::graph_call_scope &)
                  -> wh::core::result<wh::compose::graph_stream_reader> {
                auto typed = read_any<int>(input);
                if (typed.has_error()) {
                  return wh::core::result<wh::compose::graph_stream_reader>::failure(typed.error());
                }
                return make_int_graph_stream({typed.value(), typed.value() + 1, typed.value() + 2},
                                             4U);
              })
          .has_value());
  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::stream, wh::compose::node_contract::value>(
                  "left",
                  [](wh::compose::graph_stream_reader input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    auto values = collect_int_graph_stream(std::move(input));
                    if (values.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(values.error());
                    }
                    return wh::core::any(sum_ints(values.value()));
                  })
              .has_value());
  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::stream, wh::compose::node_contract::stream>(
                  "right_transform",
                  [](wh::compose::graph_stream_reader input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_stream_reader> {
                    auto values = collect_int_graph_stream(std::move(input));
                    if (values.has_error()) {
                      return wh::core::result<wh::compose::graph_stream_reader>::failure(
                          values.error());
                    }
                    auto [writer, reader] = wh::compose::make_graph_stream(4U);
                    for (const auto value : values.value()) {
                      auto pushed = writer.try_write(wh::core::any(value * 10));
                      if (pushed.has_error()) {
                        return wh::core::result<wh::compose::graph_stream_reader>::failure(
                            pushed.error());
                      }
                    }
                    auto closed = writer.close();
                    if (closed.has_error()) {
                      return wh::core::result<wh::compose::graph_stream_reader>::failure(
                          closed.error());
                    }
                    return std::move(reader);
                  })
              .has_value());
  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::stream, wh::compose::node_contract::value>(
                  "right",
                  [](wh::compose::graph_stream_reader input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    auto values = collect_int_graph_stream(std::move(input));
                    if (values.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(values.error());
                    }
                    return wh::core::any(sum_ints(values.value()));
                  })
              .has_value());
  REQUIRE(graph
              .add_lambda(
                  "join",
                  [](const wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    auto merged = read_any<wh::compose::graph_value_map>(input);
                    if (merged.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(merged.error());
                    }
                    auto left = read_any<int>(merged.value().at("left"));
                    auto right = read_any<int>(merged.value().at("right"));
                    if (left.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(left.error());
                    }
                    if (right.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(right.error());
                    }
                    return wh::core::any(left.value() + right.value());
                  })
              .has_value());

  REQUIRE(graph.add_entry_edge("source").has_value());
  REQUIRE(graph.add_edge("source", "left").has_value());
  REQUIRE(graph.add_edge("source", "right_transform").has_value());
  REQUIRE(graph.add_edge("right_transform", "right").has_value());
  REQUIRE(graph.add_edge("left", "join").has_value());
  REQUIRE(graph.add_edge("right", "join").has_value());
  REQUIRE(graph.add_exit_edge("join").has_value());
  REQUIRE(graph.compile().has_value());

  for (int iteration = 0; iteration < 128; ++iteration) {
    wh::core::run_context context{};
    auto invoked = invoke_value_sync(graph, wh::core::any(iteration), context);
    REQUIRE(invoked.has_value());
    auto typed = read_any<int>(invoked.value());
    REQUIRE(typed.has_value());
    REQUIRE(typed.value() == 33 * iteration + 33);
  }
}

TEST_CASE("compose graph value fan-in requires eof before materializing value",
          "[core][compose][graph][boundary]") {
  auto build_case = [](int &join_value, const bool close_second_stream,
                       const std::optional<std::chrono::milliseconds> node_timeout =
                           std::nullopt) -> wh::compose::graph {
    wh::compose::graph_compile_options options{};
    options.mode = wh::compose::graph_runtime_mode::dag;
    options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
    options.fan_in_policy = wh::compose::graph_fan_in_policy::require_all_sources;
    options.node_timeout = node_timeout;
    wh::compose::graph graph{std::move(options)};

    REQUIRE(graph
                .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream>(
                    "stream_a",
                    [](const wh::compose::graph_value &, wh::core::run_context &,
                       const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_stream_reader> {
                      return wh::compose::make_single_value_stream_reader(10);
                    })
                .has_value());
    REQUIRE(graph
                .add_lambda<wh::compose::node_contract::value, wh::compose::node_contract::stream>(
                    "stream_b",
                    [close_second_stream](const wh::compose::graph_value &, wh::core::run_context &,
                                          const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_stream_reader> {
                      if (close_second_stream) {
                        return wh::compose::make_single_value_stream_reader(20);
                      }
                      auto [writer, reader] = wh::compose::make_graph_stream(2U);
                      auto pushed = writer.try_write(wh::core::any(20));
                      if (pushed.has_error()) {
                        return wh::core::result<wh::compose::graph_stream_reader>::failure(
                            pushed.error());
                      }
                      return std::move(reader);
                    })
                .has_value());
    REQUIRE(graph
                .add_lambda(
                    "join",
                    [&join_value](const wh::compose::graph_value &input, wh::core::run_context &,
                                  const wh::compose::graph_call_scope &)
                        -> wh::core::result<wh::compose::graph_value> {
                      auto merged = read_any<wh::compose::graph_value_map>(input);
                      if (merged.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(merged.error());
                      }
                      auto left = collect_int_graph_chunk_values(merged.value().at("stream_a"));
                      auto right = collect_int_graph_chunk_values(merged.value().at("stream_b"));
                      if (left.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(left.error());
                      }
                      if (right.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(right.error());
                      }
                      join_value = sum_ints(left.value()) + sum_ints(right.value());
                      return wh::core::any(join_value);
                    })
                .has_value());

    REQUIRE(graph.add_entry_edge("stream_a").has_value());
    REQUIRE(graph.add_entry_edge("stream_b").has_value());
    REQUIRE(graph.add_edge("stream_a", "join", make_auto_contract_edge_options()).has_value());
    REQUIRE(graph.add_edge("stream_b", "join", make_auto_contract_edge_options()).has_value());
    REQUIRE(graph.add_exit_edge("join").has_value());
    REQUIRE(graph.compile().has_value());
    return graph;
  };

  int open_join_value = -1;
  auto open_stream_graph = build_case(open_join_value, false);
  int closed_join_value = -1;
  auto closed_stream_graph = build_case(closed_join_value, true);

  wh::core::run_context open_context{};
  exec::static_thread_pool pool{2U};
  stdexec::inplace_stop_source stop_source{};
  using status_t = wh::core::result<wh::compose::graph_invoke_result>;
  auto env =
      make_dual_scheduler_env(pool.get_scheduler(), pool.get_scheduler(), stop_source.get_token());
  wh::testing::helper::sender_capture<status_t> capture{};
  auto open_op =
      stdexec::connect(open_stream_graph.invoke(open_context, make_graph_request(wh::core::any(0))),
                       wh::testing::helper::sender_capture_receiver<status_t, decltype(env)>{
                           &capture,
                           env,
                       });
  stdexec::start(open_op);

  REQUIRE_FALSE(capture.ready.try_acquire_for(std::chrono::milliseconds{20}));
  REQUIRE(open_join_value == -1);

  stop_source.request_stop();
  auto open_status = require_value_capture(capture);
  REQUIRE(open_status.has_value());
  REQUIRE(open_status.value().output_status.has_error());
  REQUIRE(open_status.value().output_status.error() == wh::core::errc::canceled);

  wh::core::run_context closed_context{};
  auto closed_invoked = invoke_value_sync(closed_stream_graph, wh::core::any(0), closed_context);
  REQUIRE(closed_invoked.has_value());
  auto closed_output = read_any<int>(closed_invoked.value());
  REQUIRE(closed_output.has_value());
  REQUIRE(closed_output.value() == 30);
  REQUIRE(closed_join_value == 30);
}

TEST_CASE("compose graph value boundary rejects reader payloads",
          "[core][compose][error][condition]") {
  wh::compose::graph graph{};
  auto worker = wh::compose::make_passthrough_node("worker");
  REQUIRE(graph.add_passthrough(std::move(worker)).has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  auto [writer, reader] = wh::compose::make_graph_stream();
  REQUIRE(writer.try_write(wh::core::any(1)).has_value());
  REQUIRE(writer.close().has_value());

  wh::core::run_context context{};
  auto invoked = invoke_graph_sync(graph, wh::core::any(std::move(reader)), context);
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value().output_status.has_error());
  REQUIRE(invoked.value().output_status.error() == wh::core::errc::contract_violation);
}

TEST_CASE("compose graph value contract rejects move-only dynamic outputs",
          "[core][compose][graph][boundary]") {
  wh::compose::graph graph{};
  REQUIRE(graph
              .add_lambda("worker",
                          [](const wh::compose::graph_value &, wh::core::run_context &,
                             const wh::compose::graph_call_scope &)
                              -> wh::core::result<wh::compose::graph_value> {
                            return wh::core::any(std::make_unique<int>(7));
                          })
              .has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context context{};
  auto invoked = invoke_graph_sync(graph, wh::core::any(1), context);
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value().output_status.has_error());
  REQUIRE(invoked.value().output_status.error() == wh::core::errc::contract_violation);
}
