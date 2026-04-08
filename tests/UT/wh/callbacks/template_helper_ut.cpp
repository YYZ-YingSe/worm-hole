#include <catch2/catch_test_macros.hpp>

#include <type_traits>
#include <vector>

#include "wh/callbacks/manager.hpp"
#include "wh/callbacks/template_helper.hpp"

namespace {

using with_stage_callback_t =
    decltype([](const wh::callbacks::stage, const int &,
                const wh::callbacks::run_info &) {});
using without_stage_callback_t =
    decltype([](const int &, const wh::callbacks::run_info &) {});
using invalid_typed_callback_t = decltype([](double) {});

static_assert(
    wh::callbacks::TypedStageCallbackWithStage<with_stage_callback_t, int>);
static_assert(
    wh::callbacks::TypedStageCallbackWithoutStage<without_stage_callback_t, int>);
static_assert(
    !wh::callbacks::TypedStageCallbackWithStage<invalid_typed_callback_t, int>);
static_assert(!wh::callbacks::TypedStageCallbackWithoutStage<
              invalid_typed_callback_t, int>);

auto start_only_config() -> wh::callbacks::callback_config {
  return wh::callbacks::make_callback_config(
      [](const wh::callbacks::stage stage) noexcept {
        return stage == wh::callbacks::stage::start;
      });
}

} // namespace

TEST_CASE("template helper typed stage callback ignores mismatched payload types",
          "[UT][wh/callbacks/template_helper.hpp][make_typed_stage_callback][condition][branch][boundary]") {
  std::vector<int> observed{};
  auto callback = wh::callbacks::make_typed_stage_callback<int>(
      [&observed](const wh::callbacks::stage stage, const int &value,
                  const wh::callbacks::run_info &) {
        observed.push_back(static_cast<int>(stage) * 10 + value);
      });
  const wh::callbacks::run_info info{};

  callback(wh::callbacks::stage::start, wh::callbacks::make_event_view(observed),
           info);
  callback(wh::callbacks::stage::error, wh::callbacks::make_event_view(*&observed.emplace_back(3)),
           info);

  REQUIRE(observed == std::vector<int>{3, 23});
}

TEST_CASE("template helper make_typed_stage_callbacks installs the same typed callback for every stage",
          "[UT][wh/callbacks/template_helper.hpp][make_typed_stage_callbacks][condition][branch][boundary]") {
  std::vector<wh::callbacks::stage> stages{};
  auto callbacks = wh::callbacks::make_typed_stage_callbacks<int>(
      [&stages](const wh::callbacks::stage stage, const int &,
                const wh::callbacks::run_info &) { stages.push_back(stage); });
  auto payload = static_cast<int>(stages.size());
  const auto event = wh::callbacks::make_event_view(payload);
  const wh::callbacks::run_info info{};

  REQUIRE(static_cast<bool>(callbacks.on_start));
  REQUIRE(static_cast<bool>(callbacks.on_end));
  REQUIRE(static_cast<bool>(callbacks.on_error));
  REQUIRE(static_cast<bool>(callbacks.on_stream_start));
  REQUIRE(static_cast<bool>(callbacks.on_stream_end));

  callbacks.on_start(wh::callbacks::stage::start, event, info);
  callbacks.on_end(wh::callbacks::stage::end, event, info);
  callbacks.on_error(wh::callbacks::stage::error, event, info);
  callbacks.on_stream_start(wh::callbacks::stage::stream_start, event, info);
  callbacks.on_stream_end(wh::callbacks::stage::stream_end, event, info);

  REQUIRE(stages == std::vector<wh::callbacks::stage>{
                        wh::callbacks::stage::start,
                        wh::callbacks::stage::end,
                        wh::callbacks::stage::error,
                        wh::callbacks::stage::stream_start,
                        wh::callbacks::stage::stream_end,
                    });
}

TEST_CASE("template helper register_typed_local_callbacks covers moved and copied run-context inputs",
          "[UT][wh/callbacks/template_helper.hpp][register_typed_local_callbacks][condition][branch][boundary]") {
  std::vector<int> moved_hits{};
  wh::core::run_context moved_context{};
  moved_context.callbacks.emplace();
  auto moved = wh::callbacks::register_typed_local_callbacks<int>(
      std::move(moved_context), start_only_config(),
      [&moved_hits](const int &value, const wh::callbacks::run_info &) {
        moved_hits.push_back(value);
      });
  REQUIRE(moved.has_value());
  wh::core::inject_callback_event(moved.value(), wh::core::callback_stage::start,
                                  3, wh::core::callback_run_info{});
  wh::core::inject_callback_event(moved.value(), wh::core::callback_stage::end,
                                  5, wh::core::callback_run_info{});
  REQUIRE(moved_hits == std::vector<int>{3});

  std::vector<int> copied_hits{};
  wh::core::run_context copied_context{};
  copied_context.callbacks.emplace();
  auto copied = wh::callbacks::register_typed_local_callbacks<int>(
      copied_context, start_only_config(),
      [&copied_hits](const int &value, const wh::callbacks::run_info &) {
        copied_hits.push_back(value * 2);
      });
  REQUIRE(copied.has_value());
  wh::core::inject_callback_event(copied.value(), wh::core::callback_stage::start,
                                  4, wh::core::callback_run_info{});
  REQUIRE(copied_hits == std::vector<int>{8});
}
