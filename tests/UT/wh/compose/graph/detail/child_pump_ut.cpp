#include <memory>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "wh/compose/graph/detail/child_pump.hpp"
#include "wh/compose/node/execution.hpp"

namespace {

struct counting_child_pump_policy {
  using child_sender_type = wh::compose::graph_sender;
  using completion_type = wh::core::result<wh::compose::graph_value>;

  std::shared_ptr<std::vector<int>> seen{};
  std::shared_ptr<bool> cleaned{};
  int next{0};

  [[nodiscard]] auto next_step()
      -> wh::core::result<wh::compose::detail::child_pump_step<child_sender_type>> {
    if (next == 0) {
      ++next;
      return wh::compose::detail::child_pump_step<child_sender_type>::launch(
          wh::compose::detail::bridge_graph_sender(stdexec::just(
              wh::core::result<wh::compose::graph_value>{wh::compose::graph_value{1}})));
    }
    if (next == 1) {
      ++next;
      return wh::compose::detail::child_pump_step<child_sender_type>::launch(
          wh::compose::detail::bridge_graph_sender(stdexec::just(
              wh::core::result<wh::compose::graph_value>{wh::compose::graph_value{2}})));
    }
    return wh::compose::detail::child_pump_step<child_sender_type>::finish_with(
        wh::compose::graph_value{static_cast<int>(seen->size())});
  }

  [[nodiscard]] auto handle_completion(completion_type current)
      -> std::optional<wh::core::result<wh::compose::graph_value>> {
    if (current.has_error()) {
      return wh::core::result<wh::compose::graph_value>::failure(current.error());
    }
    auto *typed = wh::core::any_cast<int>(&current.value());
    REQUIRE(typed != nullptr);
    seen->push_back(*typed);
    return std::nullopt;
  }

  auto cleanup() noexcept -> void { *cleaned = true; }
};

struct failing_child_pump_policy {
  using child_sender_type = wh::compose::graph_sender;
  using completion_type = wh::core::result<wh::compose::graph_value>;

  int next{0};

  [[nodiscard]] auto next_step()
      -> wh::core::result<wh::compose::detail::child_pump_step<child_sender_type>> {
    if (next++ == 0) {
      return wh::compose::detail::child_pump_step<child_sender_type>::launch(
          wh::compose::detail::bridge_graph_sender(
              stdexec::just(wh::core::result<wh::compose::graph_value>::failure(
                  wh::core::errc::invalid_argument))));
    }
    return wh::compose::detail::child_pump_step<child_sender_type>::finish_with(
        wh::compose::detail::make_graph_unit_value());
  }

  [[nodiscard]] auto handle_completion(completion_type current)
      -> std::optional<wh::core::result<wh::compose::graph_value>> {
    if (current.has_error()) {
      return wh::core::result<wh::compose::graph_value>::failure(current.error());
    }
    return std::nullopt;
  }
};

} // namespace

TEST_CASE("child pump sender serializes child completions and runs cleanup on finish",
          "[UT][wh/compose/graph/detail/"
          "child_pump.hpp][make_child_pump_sender][condition][branch][boundary]") {
  auto seen = std::make_shared<std::vector<int>>();
  auto cleaned = std::make_shared<bool>(false);

  auto sender = wh::compose::detail::make_child_pump_sender(
      counting_child_pump_policy{
          .seen = seen,
          .cleaned = cleaned,
      },
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}));
  auto waited = stdexec::sync_wait(std::move(sender));
  REQUIRE(waited.has_value());
  REQUIRE(std::get<0>(*waited).has_value());
  REQUIRE(*wh::core::any_cast<int>(&std::get<0>(*waited).value()) == 2);
  REQUIRE(*cleaned);
  REQUIRE(*seen == std::vector<int>{1, 2});
}

TEST_CASE(
    "child pump sender propagates child terminal failures",
    "[UT][wh/compose/graph/detail/child_pump.hpp][child_pump_sender::connect][branch][error]") {
  auto sender = wh::compose::detail::make_child_pump_sender(
      failing_child_pump_policy{},
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}));
  auto waited = stdexec::sync_wait(std::move(sender));
  REQUIRE(waited.has_value());
  REQUIRE(std::get<0>(*waited).has_error());
  REQUIRE(std::get<0>(*waited).error() == wh::core::errc::invalid_argument);
}
