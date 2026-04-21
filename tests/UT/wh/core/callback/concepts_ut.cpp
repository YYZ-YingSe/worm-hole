#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "wh/core/callback/concepts.hpp"

namespace {

struct config_probe {
  wh::core::callback_timing_checker timing_checker{
      [](wh::core::callback_stage) noexcept { return true; }};
  std::string name{"probe"};
};

static_assert(
    wh::core::TimingChecker<decltype([](wh::core::callback_stage) noexcept { return true; })>);
static_assert(wh::core::StageViewCallbackLike<
              decltype([](wh::core::callback_stage, wh::core::callback_event_view,
                          const wh::core::callback_run_info &) {})>);
static_assert(wh::core::StagePayloadCallbackLike<
              decltype([](wh::core::callback_stage, wh::core::callback_event_payload,
                          const wh::core::callback_run_info &) {})>);
static_assert(wh::core::CallbackConfigLike<config_probe>);
static_assert(!wh::core::CallbackConfigLike<int>);

} // namespace

TEST_CASE("callback concepts validate expected callback shapes",
          "[UT][wh/core/callback/concepts.hpp][TimingChecker][condition][branch][boundary]") {
  STATIC_REQUIRE(
      wh::core::TimingChecker<decltype([](wh::core::callback_stage) noexcept { return true; })>);
  STATIC_REQUIRE(wh::core::CallbackConfigLike<config_probe>);

  config_probe config{};
  REQUIRE(config.timing_checker(wh::core::callback_stage::start));
  REQUIRE(config.name == "probe");
}

TEST_CASE("callback concepts accept stage view and payload callbacks with expected signatures",
          "[UT][wh/core/callback/concepts.hpp][StageViewCallbackLike][condition][branch]") {
  bool view_called = false;
  bool payload_called = false;

  auto view_callback = [&](wh::core::callback_stage, wh::core::callback_event_view,
                           const wh::core::callback_run_info &) { view_called = true; };
  auto payload_callback = [&](wh::core::callback_stage, wh::core::callback_event_payload,
                              const wh::core::callback_run_info &) { payload_called = true; };

  STATIC_REQUIRE(wh::core::StageViewCallbackLike<decltype(view_callback)>);
  STATIC_REQUIRE(wh::core::StagePayloadCallbackLike<decltype(payload_callback)>);

  view_callback(wh::core::callback_stage::start, wh::core::callback_event_view{},
                wh::core::callback_run_info{});
  payload_callback(wh::core::callback_stage::end, wh::core::callback_event_payload{},
                   wh::core::callback_run_info{});
  REQUIRE(view_called);
  REQUIRE(payload_called);
}
