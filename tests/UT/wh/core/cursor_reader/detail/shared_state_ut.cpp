#include <atomic>
#include <memory>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "wh/core/cursor_reader/detail/shared_state.hpp"
#include "wh/core/intrusive_ptr.hpp"

namespace {

using result_t = wh::core::result<int>;

struct source_stats {
  std::vector<std::optional<result_t>> try_results{};
  std::size_t try_index{0U};
  std::vector<result_t> read_results{};
  std::size_t read_index{0U};
  result_t async_result{0};
  std::vector<result_t> async_results{};
  std::size_t async_index{0U};
  int close_calls{0};
  bool automatic_close{true};
};

struct scripted_async_source {
  std::shared_ptr<source_stats> stats{};

  [[nodiscard]] auto read() -> result_t {
    if (stats->read_index < stats->read_results.size()) {
      return stats->read_results[stats->read_index++];
    }
    return result_t::failure(wh::core::errc::not_found);
  }

  [[nodiscard]] auto try_read() -> std::optional<result_t> {
    if (stats->try_index < stats->try_results.size()) {
      auto &slot = stats->try_results[stats->try_index++];
      auto next = std::move(slot);
      slot.reset();
      return next;
    }
    return std::nullopt;
  }

  [[nodiscard]] auto read_async() {
    if (stats->async_index < stats->async_results.size()) {
      return stdexec::just(stats->async_results[stats->async_index++]);
    }
    return stdexec::just(stats->async_result);
  }

  [[nodiscard]] auto close() -> wh::core::result<void> {
    ++stats->close_calls;
    return {};
  }
};

struct async_probe_waiter : wh::core::cursor_reader_detail::async_waiter_base<result_t> {
  bool completed{false};

  async_probe_waiter() {
    static constexpr
        typename wh::core::cursor_reader_detail::async_waiter_base<result_t>::ops_type waiter_ops{
            [](auto *base) noexcept { static_cast<async_probe_waiter *>(base)->completed = true; }};
    this->ops = &waiter_ops;
  }
};

struct tracked_result_probe {
  static inline std::atomic<int> live_count{0};

  int value{0};

  tracked_result_probe() noexcept { live_count.fetch_add(1, std::memory_order_relaxed); }
  explicit tracked_result_probe(const int next) noexcept : value(next) {
    live_count.fetch_add(1, std::memory_order_relaxed);
  }
  tracked_result_probe(const tracked_result_probe &other) noexcept : value(other.value) {
    live_count.fetch_add(1, std::memory_order_relaxed);
  }
  tracked_result_probe(tracked_result_probe &&other) noexcept : value(other.value) {
    live_count.fetch_add(1, std::memory_order_relaxed);
  }
  auto operator=(const tracked_result_probe &) -> tracked_result_probe & = default;
  auto operator=(tracked_result_probe &&) noexcept -> tracked_result_probe & = default;
  ~tracked_result_probe() { live_count.fetch_sub(1, std::memory_order_relaxed); }
};

using tracked_result_t = wh::core::result<tracked_result_probe>;

struct tracked_source_stats {
  std::vector<std::optional<tracked_result_t>> try_results{};
  std::size_t try_index{0U};
  int close_calls{0};
};

struct tracked_async_source {
  std::shared_ptr<tracked_source_stats> stats{};

  [[nodiscard]] auto read() -> tracked_result_t {
    return tracked_result_t::failure(wh::core::errc::not_found);
  }

  [[nodiscard]] auto try_read() -> std::optional<tracked_result_t> {
    if (stats->try_index < stats->try_results.size()) {
      auto &slot = stats->try_results[stats->try_index++];
      auto next = std::move(slot);
      slot.reset();
      return next;
    }
    return std::nullopt;
  }

  [[nodiscard]] auto read_async() {
    return stdexec::just(tracked_result_t::failure(wh::core::errc::not_found));
  }

  [[nodiscard]] auto close() -> wh::core::result<void> {
    ++stats->close_calls;
    return {};
  }
};

} // namespace

TEST_CASE("shared state reuses published try-read results across readers and closes one reader "
          "without affecting others",
          "[UT][wh/core/cursor_reader/detail/"
          "shared_state.hpp][shared_state::try_read_for][condition][branch]") {
  using policy_t = wh::core::cursor_reader_detail::default_policy<scripted_async_source>;
  auto stats = std::make_shared<source_stats>();
  stats->try_results = {std::optional<result_t>{result_t{3}}};

  auto state = wh::core::detail::make_intrusive<
      wh::core::cursor_reader_detail::shared_state<scripted_async_source, policy_t>>(
      scripted_async_source{stats}, 2U);

  auto first = state->try_read_for(0U);
  auto second = state->try_read_for(1U);
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  REQUIRE(first->has_value());
  REQUIRE(second->has_value());
  REQUIRE(first->value() == 3);
  REQUIRE(second->value() == 3);
  REQUIRE(stats->try_index == 1U);

  state->close_reader(0U);
  REQUIRE(state->reader_is_closed(0U));
  REQUIRE_FALSE(state->reader_is_closed(1U));
  REQUIRE_FALSE(state->is_source_closed());
  REQUIRE(stats->close_calls == 0);
}

TEST_CASE("shared state register_async_waiter remove_async_waiter and start_async_pull cover async "
          "waiter lifecycle",
          "[UT][wh/core/cursor_reader/detail/"
          "shared_state.hpp][shared_state::register_async_waiter][branch][concurrency]") {
  using policy_t = wh::core::cursor_reader_detail::default_policy<scripted_async_source>;
  auto stats = std::make_shared<source_stats>();
  stats->try_results = {std::nullopt};
  stats->async_result = result_t{8};

  auto state = wh::core::detail::make_intrusive<
      wh::core::cursor_reader_detail::shared_state<scripted_async_source, policy_t>>(
      scripted_async_source{stats}, 1U);

  async_probe_waiter first_waiter{};
  auto first_ticket = state->register_async_waiter(0U, &first_waiter);
  REQUIRE_FALSE(first_ticket.ready.has_value());
  REQUIRE(first_ticket.registered());
  REQUIRE(first_ticket.start_pull);
  REQUIRE(first_waiter.waiting_registered());
  REQUIRE(state->remove_async_waiter(0U, &first_waiter));
  REQUIRE_FALSE(first_waiter.waiting_registered());
  REQUIRE_FALSE(state->remove_async_waiter(0U, &first_waiter));

  async_probe_waiter second_waiter{};
  auto second_ticket = state->register_async_waiter(0U, &second_waiter);
  REQUIRE_FALSE(second_ticket.ready.has_value());
  REQUIRE(second_ticket.registered());
  REQUIRE(second_ticket.start_pull);
  REQUIRE(second_waiter.waiting_registered());

  state->start_async_pull(wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}));

  REQUIRE(second_waiter.completed);
  REQUIRE_FALSE(second_waiter.waiting_registered());
  auto ready = second_waiter.take_ready();
  REQUIRE(ready.has_value());
  REQUIRE(ready.value() == 8);
}

TEST_CASE("shared state async pull remains reusable across consecutive inline completions",
          "[UT][wh/core/cursor_reader/detail/"
          "shared_state.hpp][shared_state::start_async_pull][lifetime][regression]") {
  using policy_t = wh::core::cursor_reader_detail::default_policy<scripted_async_source>;
  auto stats = std::make_shared<source_stats>();
  stats->try_results = {std::nullopt, std::nullopt};
  stats->async_results = {result_t{12}, result_t{34}};

  auto state = wh::core::detail::make_intrusive<
      wh::core::cursor_reader_detail::shared_state<scripted_async_source, policy_t>>(
      scripted_async_source{stats}, 1U);

  async_probe_waiter first_waiter{};
  auto first_ticket = state->register_async_waiter(0U, &first_waiter);
  REQUIRE(first_ticket.registered());
  REQUIRE(first_ticket.start_pull);

  state->start_async_pull(wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}));

  REQUIRE(first_waiter.completed);
  REQUIRE_FALSE(first_waiter.waiting_registered());
  auto first_ready = first_waiter.take_ready();
  REQUIRE(first_ready.has_value());
  REQUIRE(first_ready.value() == 12);

  async_probe_waiter second_waiter{};
  auto second_ticket = state->register_async_waiter(0U, &second_waiter);
  REQUIRE(second_ticket.registered());
  REQUIRE(second_ticket.start_pull);

  state->start_async_pull(wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}));

  REQUIRE(second_waiter.completed);
  REQUIRE_FALSE(second_waiter.waiting_registered());
  auto second_ready = second_waiter.take_ready();
  REQUIRE(second_ready.has_value());
  REQUIRE(second_ready.value() == 34);
}

TEST_CASE("shared state read_for uses blocking leader path and respects automatic close toggle",
          "[UT][wh/core/cursor_reader/detail/"
          "shared_state.hpp][shared_state::read_for][branch][boundary]") {
  using policy_t = wh::core::cursor_reader_detail::default_policy<scripted_async_source>;
  auto stats = std::make_shared<source_stats>();
  stats->read_results = {result_t{11}};

  auto state = wh::core::detail::make_intrusive<
      wh::core::cursor_reader_detail::shared_state<scripted_async_source, policy_t>>(
      scripted_async_source{stats}, 1U);
  state->set_automatic_close(false);

  auto blocking = state->read_for(0U);
  REQUIRE(blocking.has_value());
  REQUIRE(blocking.value() == 11);

  state->close_reader(0U);
  REQUIRE(state->reader_is_closed(0U));
  REQUIRE_FALSE(state->is_source_closed());
  REQUIRE(stats->close_calls == 0);

  async_probe_waiter waiter{};
  auto closed_ticket = state->register_async_waiter(0U, &waiter);
  REQUIRE(closed_ticket.ready.has_value());
  REQUIRE(closed_ticket.ready->has_error());
  REQUIRE(closed_ticket.ready->error() == wh::core::errc::channel_closed);
  REQUIRE_FALSE(closed_ticket.start_pull);
  REQUIRE_FALSE(waiter.waiting_registered());
}

TEST_CASE("shared state preserves lagging readers when retained storage grows after prefix reclaim",
          "[UT][wh/core/cursor_reader/detail/"
          "shared_state.hpp][shared_state::try_read_for][lifetime][regression]") {
  using policy_t = wh::core::cursor_reader_detail::default_policy<scripted_async_source>;
  auto stats = std::make_shared<source_stats>();
  stats->try_results = {
      std::optional<result_t>{result_t{0}}, std::optional<result_t>{result_t{1}},
      std::optional<result_t>{result_t{2}}, std::optional<result_t>{result_t{3}},
      std::optional<result_t>{result_t{4}}, std::optional<result_t>{result_t{5}},
  };

  auto state = wh::core::detail::make_intrusive<
      wh::core::cursor_reader_detail::shared_state<scripted_async_source, policy_t>>(
      scripted_async_source{stats}, 2U);

  auto reader0_value0 = state->try_read_for(0U);
  auto reader1_value0 = state->try_read_for(1U);
  auto reader0_value1 = state->try_read_for(0U);
  auto reader0_value2 = state->try_read_for(0U);
  auto reader0_value3 = state->try_read_for(0U);
  auto reader0_value4 = state->try_read_for(0U);
  auto reader0_value5 = state->try_read_for(0U);

  REQUIRE(reader0_value0.has_value());
  REQUIRE(reader1_value0.has_value());
  REQUIRE(reader0_value1.has_value());
  REQUIRE(reader0_value2.has_value());
  REQUIRE(reader0_value3.has_value());
  REQUIRE(reader0_value4.has_value());
  REQUIRE(reader0_value5.has_value());
  REQUIRE(reader0_value0->value() == 0);
  REQUIRE(reader1_value0->value() == 0);
  REQUIRE(reader0_value1->value() == 1);
  REQUIRE(reader0_value2->value() == 2);
  REQUIRE(reader0_value3->value() == 3);
  REQUIRE(reader0_value4->value() == 4);
  REQUIRE(reader0_value5->value() == 5);

  for (int expected = 1; expected <= 5; ++expected) {
    auto lagging = state->try_read_for(1U);
    REQUIRE(lagging.has_value());
    REQUIRE(lagging->has_value());
    REQUIRE(lagging->value() == expected);
  }
  REQUIRE(stats->try_index == 6U);
}

TEST_CASE("shared state destroys retained unread results on scope exit",
          "[UT][wh/core/cursor_reader/detail/"
          "shared_state.hpp][shared_state::~shared_state][lifetime][regression]") {
  using tracked_policy_t = wh::core::cursor_reader_detail::default_policy<tracked_async_source>;
  tracked_result_probe::live_count.store(0, std::memory_order_release);

  auto stats = std::make_shared<tracked_source_stats>();
  stats->try_results = {std::optional<tracked_result_t>{tracked_result_t{tracked_result_probe{5}}}};

  {
    auto state = wh::core::detail::make_intrusive<
        wh::core::cursor_reader_detail::shared_state<tracked_async_source, tracked_policy_t>>(
        tracked_async_source{stats}, 2U);

    {
      auto first = state->try_read_for(0U);
      REQUIRE(first.has_value());
      REQUIRE(first->has_value());
      REQUIRE(first->value().value == 5);
    }

    REQUIRE(tracked_result_probe::live_count.load(std::memory_order_acquire) >= 1);
  }

  REQUIRE(tracked_result_probe::live_count.load(std::memory_order_acquire) == 0);
}
