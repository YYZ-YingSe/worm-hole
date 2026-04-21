#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "wh/core/run_context.hpp"

TEST_CASE("run_context session helpers cover set lookup mutation and consume",
          "[UT][wh/core/run_context.hpp][set_session_value][branch][boundary]") {
  wh::core::run_context context{};

  REQUIRE(wh::core::set_session_value(context, "count", 7).has_value());
  REQUIRE(wh::core::set_session_value(context, "name", std::string{"alpha"}).has_value());

  const auto count_const = wh::core::session_value_ref<int>(std::as_const(context), "count");
  REQUIRE(count_const.has_value());
  REQUIRE(count_const.value().get() == 7);

  auto count_mutable = wh::core::session_value_ref<int>(context, "count");
  REQUIRE(count_mutable.has_value());
  count_mutable.value().get() = 9;

  const auto moved = wh::core::consume_session_value<std::string>(context, "name");
  REQUIRE(moved.has_value());
  REQUIRE(moved.value() == "alpha");

  REQUIRE(wh::core::session_value_ref<std::string>(context, "name").error() ==
          wh::core::errc::not_found);
  REQUIRE(wh::core::session_value_ref<double>(context, "count").error() ==
          wh::core::errc::type_mismatch);
}

TEST_CASE("run_context callback registration and injection respect availability",
          "[UT][wh/core/run_context.hpp][register_local_callbacks][branch][boundary]") {
  wh::core::run_context missing_callbacks{};
  wh::core::stage_callbacks callbacks{};
  callbacks.on_end = [](wh::core::callback_stage, wh::core::callback_event_view,
                        const wh::core::callback_run_info &) {};

  auto missing = wh::core::register_local_callbacks(
      std::move(missing_callbacks),
      wh::core::callback_config{
          .timing_checker = [](wh::core::callback_stage) noexcept { return true; },
          .name = "missing"},
      callbacks);
  REQUIRE(missing.has_error());
  REQUIRE(missing.error() == wh::core::errc::not_found);

  wh::core::run_context context{};
  REQUIRE_FALSE(wh::core::has_callback_manager(context));
  context.callbacks.emplace();
  REQUIRE(wh::core::has_callback_manager(context));

  std::vector<int> observed{};
  callbacks.on_end = [&observed](wh::core::callback_stage,
                                 const wh::core::callback_event_view event,
                                 const wh::core::callback_run_info &) {
    const auto *typed = event.get_if<int>();
    REQUIRE(typed != nullptr);
    observed.push_back(*typed);
  };

  auto updated = wh::core::register_local_callbacks(
      std::move(context),
      [](wh::core::callback_stage stage) noexcept {
        return stage == wh::core::callback_stage::end;
      },
      callbacks, "local");
  REQUIRE(updated.has_value());

  wh::core::inject_callback_event(updated.value(), wh::core::callback_stage::start, 1,
                                  wh::core::callback_run_info{});
  wh::core::inject_callback_event(updated.value(), wh::core::callback_stage::end, 7,
                                  wh::core::callback_run_info{});
  REQUIRE(observed == std::vector<int>{7});
}

TEST_CASE("run_context fatal helpers capture errors and optionally emit callback events",
          "[UT][wh/core/run_context.hpp][run_with_fatal_error_capture][branch]") {
  {
    try {
      throw std::runtime_error{"boom"};
    } catch (...) {
      const auto fatal = wh::core::capture_fatal_error();
      REQUIRE(fatal.code == wh::core::errc::internal_error);
      REQUIRE(fatal.exception_message == "boom");
    }
  }

  const auto ok = wh::core::run_with_fatal_error_capture([] {});
  REQUIRE(ok.has_value());

  const auto failed =
      wh::core::run_with_fatal_error_capture([] { throw std::runtime_error{"broken"}; });
  REQUIRE(failed.has_error());
  REQUIRE(failed.error().code == wh::core::errc::internal_error);
  REQUIRE(failed.error().exception_message == "broken");

  wh::core::run_context context{};
  context.callbacks.emplace();
  std::vector<std::string> seen{};
  wh::core::stage_callbacks callbacks{};
  callbacks.on_error = [&seen](wh::core::callback_stage, const wh::core::callback_event_view event,
                               const wh::core::callback_run_info &) {
    const auto *typed = event.get_if<wh::core::callback_fatal_error>();
    REQUIRE(typed != nullptr);
    seen.push_back(typed->exception_message);
  };
  context.callbacks->manager.register_local_callbacks(
      wh::core::callback_config{.timing_checker =
                                    [](wh::core::callback_stage stage) noexcept {
                                      return stage == wh::core::callback_stage::error;
                                    },
                                .name = "fatal"},
      callbacks);

  wh::core::run_with_fatal_event(context, [] { throw std::runtime_error{"fatal"}; });
  REQUIRE(seen == std::vector<std::string>{"fatal"});
}

TEST_CASE("run_context const callback registration returns copied context and missing injection is "
          "a no-op",
          "[UT][wh/core/run_context.hpp][register_local_callbacks][condition][boundary]") {
  wh::core::run_context context{};
  context.callbacks.emplace();

  std::vector<int> observed{};
  wh::core::stage_callbacks callbacks{};
  callbacks.on_start = [&observed](wh::core::callback_stage,
                                   const wh::core::callback_event_view event,
                                   const wh::core::callback_run_info &) {
    const auto *typed = event.get_if<int>();
    REQUIRE(typed != nullptr);
    observed.push_back(*typed);
  };

  auto copied = wh::core::register_local_callbacks(
      std::as_const(context),
      [](wh::core::callback_stage stage) noexcept {
        return stage == wh::core::callback_stage::start;
      },
      callbacks, "copied");
  REQUIRE(copied.has_value());

  wh::core::inject_callback_event(context, wh::core::callback_stage::start, 1,
                                  wh::core::callback_run_info{});
  REQUIRE(observed.empty());

  wh::core::inject_callback_event(copied.value(), wh::core::callback_stage::start, 3,
                                  wh::core::callback_run_info{});
  REQUIRE(observed == std::vector<int>{3});

  wh::core::run_context missing{};
  wh::core::inject_callback_event(missing, wh::core::callback_stage::start, 7,
                                  wh::core::callback_run_info{});
  REQUIRE(observed == std::vector<int>{3});
}
