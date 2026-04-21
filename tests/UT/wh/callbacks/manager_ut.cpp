#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "wh/callbacks/manager.hpp"
#include "wh/internal/callbacks.hpp"

static_assert(std::same_as<wh::callbacks::manager, wh::internal::callback_manager>);
static_assert(
    std::same_as<wh::callbacks::stage_registration, wh::callbacks::manager::stage_registration>);
static_assert(
    std::same_as<wh::callbacks::registration_list, wh::callbacks::manager::registration_list>);

TEST_CASE("callback manager make_callback_config preserves checker and debug name",
          "[UT][wh/callbacks/manager.hpp][make_callback_config][condition][branch][boundary]") {
  const auto start_only = wh::callbacks::make_callback_config(
      [](const wh::callbacks::stage stage) noexcept {
        return stage == wh::callbacks::stage::start;
      },
      "start-only");

  REQUIRE(start_only.name == "start-only");
  REQUIRE(start_only.timing_checker(wh::callbacks::stage::start));
  REQUIRE_FALSE(start_only.timing_checker(wh::callbacks::stage::end));
}

TEST_CASE("callback manager merge_callback_config intersects stage filters and keeps last name",
          "[UT][wh/callbacks/manager.hpp][merge_callback_config][condition][branch][boundary]") {
  const auto start_only = wh::callbacks::make_callback_config(
      [](const wh::callbacks::stage stage) noexcept {
        return stage == wh::callbacks::stage::start;
      },
      "start-only");
  const auto end_only = wh::callbacks::make_callback_config(
      [](const wh::callbacks::stage stage) noexcept { return stage == wh::callbacks::stage::end; },
      "end-only");

  const auto merged = wh::callbacks::merge_callback_config(
      start_only, wh::callbacks::make_callback_config(
                      [](const wh::callbacks::stage) noexcept { return true; }, "all"));
  const auto disjoint = wh::callbacks::merge_callback_config(start_only, end_only);

  REQUIRE(merged.timing_checker(wh::callbacks::stage::start));
  REQUIRE_FALSE(merged.timing_checker(wh::callbacks::stage::end));
  REQUIRE(merged.name == "all");

  REQUIRE_FALSE(disjoint.timing_checker(wh::callbacks::stage::start));
  REQUIRE_FALSE(disjoint.timing_checker(wh::callbacks::stage::end));
  REQUIRE(disjoint.name == "end-only");
}

TEST_CASE("callback manager merge_callback_config defaults to accept-all when inputs omit checkers",
          "[UT][wh/callbacks/manager.hpp][merge_callback_config][condition][branch][boundary]") {
  const auto merged = wh::callbacks::merge_callback_config(wh::callbacks::callback_config{},
                                                           wh::callbacks::callback_config{});

  REQUIRE(merged.timing_checker(wh::callbacks::stage::start));
  REQUIRE(merged.timing_checker(wh::callbacks::stage::end));
  REQUIRE(merged.timing_checker(wh::callbacks::stage::error));
}
