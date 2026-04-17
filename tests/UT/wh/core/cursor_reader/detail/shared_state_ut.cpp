#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/core/intrusive_ptr.hpp"
#include "wh/core/cursor_reader/detail/shared_state.hpp"

namespace {

using result_t = wh::core::result<int>;

struct source_stats {
  std::vector<std::optional<result_t>> try_results{};
  std::size_t try_index{0U};
  std::vector<result_t> read_results{};
  std::size_t read_index{0U};
  result_t async_result{0};
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
      return stats->try_results[stats->try_index++];
    }
    return std::nullopt;
  }

  [[nodiscard]] auto read_async() { return stdexec::just(stats->async_result); }

  [[nodiscard]] auto close() -> wh::core::result<void> {
    ++stats->close_calls;
    return {};
  }
};

struct async_probe_waiter : wh::core::cursor_reader_detail::async_waiter_base<result_t> {
  bool completed{false};

  async_probe_waiter() {
    static constexpr typename wh::core::cursor_reader_detail::async_waiter_base<
        result_t>::ops_type ops{
        [](auto *base) noexcept {
          static_cast<async_probe_waiter *>(base)->completed = true;
        }};
    this->ops = &ops;
  }
};

} // namespace

TEST_CASE("shared state reuses published try-read results across readers and closes one reader without affecting others",
          "[UT][wh/core/cursor_reader/detail/shared_state.hpp][shared_state::try_read_for][condition][branch]") {
  using policy_t =
      wh::core::cursor_reader_detail::default_policy<scripted_async_source>;
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

TEST_CASE("shared state register_async_waiter remove_async_waiter and start_async_pull cover async waiter lifecycle",
          "[UT][wh/core/cursor_reader/detail/shared_state.hpp][shared_state::register_async_waiter][branch][concurrency]") {
  using policy_t =
      wh::core::cursor_reader_detail::default_policy<scripted_async_source>;
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

  state->start_async_pull(
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}));

  REQUIRE(second_waiter.completed);
  REQUIRE_FALSE(second_waiter.waiting_registered());
  auto ready = second_waiter.take_ready();
  REQUIRE(ready.has_value());
  REQUIRE(ready.value() == 8);
}

TEST_CASE("shared state read_for uses blocking leader path and respects automatic close toggle",
          "[UT][wh/core/cursor_reader/detail/shared_state.hpp][shared_state::read_for][branch][boundary]") {
  using policy_t =
      wh::core::cursor_reader_detail::default_policy<scripted_async_source>;
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
