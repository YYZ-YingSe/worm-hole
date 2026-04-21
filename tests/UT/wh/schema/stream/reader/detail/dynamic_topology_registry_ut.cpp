#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "helper/thread_support.hpp"
#include "wh/schema/stream/pipe.hpp"
#include "wh/schema/stream/reader/merge_stream_reader.hpp"
#include "wh/schema/stream/reader/values_stream_reader.hpp"

namespace {

struct test_waiter {
  test_waiter *next{nullptr};
  test_waiter *prev{nullptr};
  const wh::schema::stream::detail::waiter_ops<test_waiter> *ops{nullptr};
  int id{0};
  std::vector<int> *completed{nullptr};
};

} // namespace

TEST_CASE("dynamic topology registry helper waiters preserve intrusive ordering and notification semantics",
          "[UT][wh/schema/stream/reader/detail/dynamic_topology_registry.hpp][intrusive_waiter_list::push_back][branch][boundary]") {
  wh::schema::stream::detail::intrusive_waiter_list<test_waiter> waiters{};
  std::vector<int> completed{};
  static const wh::schema::stream::detail::waiter_ops<test_waiter> ops{
      .complete = [](test_waiter *waiter) noexcept {
        waiter->completed->push_back(waiter->id);
      }};

  test_waiter first{.ops = &ops, .id = 1, .completed = &completed};
  test_waiter second{.ops = &ops, .id = 2, .completed = &completed};
  test_waiter third{.ops = &ops, .id = 3, .completed = &completed};

  waiters.push_back(&first);
  waiters.push_back(&second);
  waiters.push_back(&third);
  REQUIRE(waiters.try_remove(&second));
  REQUIRE_FALSE(waiters.try_remove(&second));
  REQUIRE(waiters.try_pop_front() == &first);
  REQUIRE(waiters.try_pop_front() == &third);
  REQUIRE(waiters.try_pop_front() == nullptr);

  wh::schema::stream::detail::waiter_ready_list<test_waiter> ready{};
  ready.push_back(&first);
  ready.push_back(&second);
  ready.complete_all();
  REQUIRE(completed == std::vector<int>({1, 2}));

  wh::schema::stream::detail::topology_sync_waiter sync_waiter{};
  std::atomic<bool> resumed{false};
  wh::testing::helper::joining_thread thread([&] {
    sync_waiter.wait();
    resumed.store(true, std::memory_order_release);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds{10});
  sync_waiter.notify();
  thread.join();
  REQUIRE(resumed.load(std::memory_order_acquire));
}

TEST_CASE("dynamic topology registry covers pending attach disable immediate read and round lifecycle",
          "[UT][wh/schema/stream/reader/detail/dynamic_topology_registry.hpp][dynamic_topology_registry::prepare_round][condition][branch][concurrency]") {
  using reader_t =
      wh::schema::stream::values_stream_reader<std::vector<int>>;
  using registry_t = wh::schema::stream::detail::dynamic_topology_registry<reader_t>;
  using lane_t = wh::schema::stream::named_stream_reader<reader_t>;
  using chunk_t = wh::schema::stream::stream_chunk<int>;
  using status_t = wh::schema::stream::stream_result<chunk_t>;

  registry_t pending{std::vector<std::string>{"A", "B", "C", "D", "E", "F"}};
  REQUIRE_FALSE(pending.uses_fixed_poll_path());
  REQUIRE(pending.has_pending_lanes());
  REQUIRE(pending.lane_count() == 6U);
  REQUIRE_FALSE(pending.is_closed());
  REQUIRE_FALSE(pending.is_source_closed());

  auto waiting_round = pending.prepare_round();
  REQUIRE(waiting_round.wait_for_topology);
  REQUIRE_FALSE(waiting_round.immediate_status.has_value());
  const auto epoch = pending.topology_epoch();

  wh::schema::stream::detail::dynamic_topology_registry<reader_t>::sync_waiter_type
      sync_waiter{};
  REQUIRE(pending.register_sync_topology_waiter(&sync_waiter, epoch));
  std::optional<wh::core::result<void>> attach_status{};
  wh::testing::helper::joining_thread attach_thread([&] {
    attach_status.emplace(pending.attach(
        "A", wh::schema::stream::make_values_stream_reader(std::vector<int>{7})));
  });
  sync_waiter.wait();
  attach_thread.join();
  REQUIRE(attach_status.has_value());
  REQUIRE(attach_status->has_value());
  REQUIRE(pending.topology_epoch() == epoch + 1U);

  auto immediate = pending.try_read_immediate();
  REQUIRE(std::holds_alternative<status_t>(immediate));
  auto first = std::move(std::get<status_t>(immediate));
  REQUIRE(first.has_value());
  REQUIRE(first.value().value == std::optional<int>{7});
  REQUIRE(first.value().source == "A");

  auto source_eof = pending.try_read_immediate();
  REQUIRE(std::holds_alternative<status_t>(source_eof));
  auto eof_chunk = std::move(std::get<status_t>(source_eof));
  REQUIRE(eof_chunk.has_value());
  REQUIRE(eof_chunk.value().eof);
  REQUIRE(eof_chunk.value().source == "A");

  REQUIRE(pending.disable("B").has_value());
  REQUIRE(pending.disable("B").has_value());
  REQUIRE(pending.disable("missing").has_error());

  auto terminal = pending.close_all();
  REQUIRE(terminal.has_value());
  REQUIRE(pending.is_closed());

  std::vector<lane_t> fixed_lanes{};
  fixed_lanes.emplace_back(
      "L0", wh::schema::stream::make_values_stream_reader(std::vector<int>{1}));
  fixed_lanes.emplace_back(
      "L1", wh::schema::stream::make_values_stream_reader(std::vector<int>{2}));
  registry_t fixed{std::move(fixed_lanes)};
  REQUIRE(fixed.uses_fixed_poll_path());
  REQUIRE(fixed.lane_count() == 2U);
  REQUIRE(fixed.is_source_closed());

  auto round = fixed.prepare_round();
  REQUIRE(round.lanes.size() == 2U);
  REQUIRE_FALSE(round.wait_for_topology);
  REQUIRE(fixed.close_all().has_error());
  fixed.complete_round_without_winner(round.lanes);
  REQUIRE(fixed.close_all().has_value());
}

TEST_CASE("dynamic topology registry complete_round_winner and async waiters resolve source eof and topology readiness",
          "[UT][wh/schema/stream/reader/detail/dynamic_topology_registry.hpp][dynamic_topology_registry::complete_round_winner][branch][concurrency]") {
  using reader_t =
      wh::schema::stream::values_stream_reader<std::vector<int>>;
  using registry_t = wh::schema::stream::detail::dynamic_topology_registry<reader_t>;
  using lane_t = wh::schema::stream::named_stream_reader<reader_t>;
  using async_waiter_t = registry_t::async_waiter_type;
  using chunk_t = wh::schema::stream::stream_chunk<int>;
  using status_t = wh::schema::stream::stream_result<chunk_t>;

  std::vector<lane_t> lanes{};
  lanes.emplace_back(
      "A", wh::schema::stream::make_values_stream_reader(std::vector<int>{5}));
  registry_t registry{std::move(lanes)};

  auto round = registry.prepare_round();
  REQUIRE(round.lanes.size() == 1U);
  auto value_resolution =
      registry.complete_round_winner(round.lanes, 0U,
                                     status_t{chunk_t::make_value(5)});
  REQUIRE(value_resolution.status.has_value());
  REQUIRE(value_resolution.status.value().value == std::optional<int>{5});
  REQUIRE(value_resolution.status.value().source == "A");
  REQUIRE_FALSE(value_resolution.close_lane.has_value());

  round = registry.prepare_round();
  REQUIRE(round.lanes.size() == 1U);
  auto eof_resolution =
      registry.complete_round_winner(round.lanes, 0U,
                                     status_t{chunk_t::make_eof()});
  REQUIRE(eof_resolution.status.has_value());
  REQUIRE(eof_resolution.status.value().eof);
  REQUIRE(eof_resolution.status.value().source == "A");
  REQUIRE(eof_resolution.close_lane.has_value());
  registry.close_lane_if_needed(eof_resolution.close_lane);

  registry_t pending{std::vector<std::string>{"A"}};
  const auto epoch = pending.topology_epoch();
  async_waiter_t removed_waiter{};
  static std::atomic<int> completed_count{0};
  static const wh::schema::stream::detail::waiter_ops<async_waiter_t> async_ops{
      .complete = [](async_waiter_t *) noexcept {
        completed_count.fetch_add(1, std::memory_order_acq_rel);
      }};
  removed_waiter.ops = &async_ops;
  REQUIRE(pending.register_async_topology_waiter(&removed_waiter, epoch));
  REQUIRE(pending.remove_async_topology_waiter(&removed_waiter));
  REQUIRE_FALSE(pending.remove_async_topology_waiter(&removed_waiter));

  completed_count.store(0, std::memory_order_release);
  async_waiter_t waiter{};
  waiter.ops = &async_ops;
  REQUIRE(pending.register_async_topology_waiter(&waiter, epoch));

  wh::schema::stream::detail::dynamic_topology_registry<reader_t>::sync_waiter_type
      wake_sync{};
  REQUIRE(pending.register_sync_topology_waiter(&wake_sync, epoch));
  wh::testing::helper::joining_thread attach_thread([&] {
    REQUIRE(pending.attach(
                "A", wh::schema::stream::make_values_stream_reader(std::vector<int>{9}))
                .has_value());
  });
  wake_sync.wait();
  attach_thread.join();
  REQUIRE(completed_count.load(std::memory_order_acquire) == 1);
}
