#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "wh/internal/callbacks.hpp"

TEST_CASE("internal callback manager tracks registrations and dispatch order",
          "[UT][wh/internal/callbacks.hpp][callback_manager::dispatch][branch][boundary]") {
  wh::internal::callback_manager manager{};

  std::vector<int> start_order{};
  wh::core::stage_callbacks first{};
  first.on_start = [&start_order](const wh::core::callback_stage,
                                  const wh::core::callback_event_view,
                                  const wh::core::callback_run_info &) {
    start_order.push_back(1);
  };
  wh::core::stage_callbacks second{};
  second.on_start = [&start_order](const wh::core::callback_stage,
                                   const wh::core::callback_event_view,
                                   const wh::core::callback_run_info &) {
    start_order.push_back(2);
  };

  manager.register_local_callbacks(
      wh::internal::make_callback_config(
          [](const wh::core::callback_stage stage) noexcept {
            return stage == wh::core::callback_stage::start;
          }),
      std::move(first));
  manager.register_global_callbacks(
      wh::internal::make_callback_config(
          [](const wh::core::callback_stage stage) noexcept {
            return stage == wh::core::callback_stage::start;
          }),
      std::move(second));

  REQUIRE(manager.local_registration_count() == 1U);
  REQUIRE(manager.global_registration_count() == 1U);

  int payload = 7;
  manager.dispatch(wh::core::callback_stage::start,
                   wh::core::make_callback_event_view(payload), {});
  REQUIRE(start_order == std::vector<int>{1, 2});

  std::vector<int> end_order{};
  wh::core::stage_callbacks local_end{};
  local_end.on_end = [&end_order](const wh::core::callback_stage,
                                  const wh::core::callback_event_view,
                                  const wh::core::callback_run_info &) {
    end_order.push_back(1);
  };
  wh::core::stage_callbacks global_end{};
  global_end.on_end = [&end_order](const wh::core::callback_stage,
                                   const wh::core::callback_event_view,
                                   const wh::core::callback_run_info &) {
    end_order.push_back(2);
  };
  manager.register_local_callbacks(
      wh::internal::make_callback_config(
          [](const wh::core::callback_stage stage) noexcept {
            return stage == wh::core::callback_stage::end;
          }),
      std::move(local_end));
  manager.register_global_callbacks(
      wh::internal::make_callback_config(
          [](const wh::core::callback_stage stage) noexcept {
            return stage == wh::core::callback_stage::end;
          }),
      std::move(global_end));

  manager.dispatch(wh::core::callback_stage::end,
                   wh::core::make_callback_event_view(payload), {});
  REQUIRE(end_order == std::vector<int>{2, 1});
}

TEST_CASE("internal callback manager dispatch_single preserves typed payloads",
          "[UT][wh/internal/callbacks.hpp][callback_manager::dispatch_single][branch]") {
  wh::internal::callback_manager manager{};
  std::vector<int> seen{};

  manager.dispatch_single(
      wh::core::callback_stage::error, 9, {},
      [&seen](const wh::core::callback_stage,
              wh::core::callback_event_payload &&payload,
              const wh::core::callback_run_info &) {
        const auto decoded = wh::core::callback_event_as<int>(std::move(payload));
        REQUIRE(decoded.has_value());
        seen.push_back(decoded.value());
      });

  REQUIRE(seen == std::vector<int>{9});
}

TEST_CASE("internal callback helpers default timing filters and ignore empty payload callbacks",
          "[UT][wh/internal/callbacks.hpp][make_callback_config][condition][branch][boundary]") {
  auto config = wh::internal::make_callback_config(
      [](const wh::core::callback_stage stage) noexcept {
        return stage == wh::core::callback_stage::stream_end;
      },
      "named");
  REQUIRE(config.name == "named");
  REQUIRE(config.timing_checker(wh::core::callback_stage::stream_end));
  REQUIRE_FALSE(config.timing_checker(wh::core::callback_stage::start));

  auto merged = wh::internal::merge_callback_config(
      wh::core::callback_config{},
      wh::internal::make_callback_config(
          [](const wh::core::callback_stage) noexcept { return true; }, "merged"));
  REQUIRE(merged.name == "merged");
  REQUIRE(merged.timing_checker(wh::core::callback_stage::error));

  wh::internal::callback_manager manager{};
  manager.dispatch_single(wh::core::callback_stage::start, 42, {},
                          wh::core::stage_payload_callback{nullptr});
  SUCCEED();
}
