#include <catch2/catch_test_macros.hpp>

#include "wh/compose/callback/adapter.hpp"

TEST_CASE("compose callback adapter returns success when hooks are not configured",
          "[UT][wh/compose/callback/adapter.hpp][callback_adapter::emit_start][condition][branch][boundary]") {
  wh::compose::callback_adapter adapter{};
  wh::core::run_context context{};

  REQUIRE(adapter.emit_start(wh::compose::graph_value{7}, context).has_value());
  REQUIRE(adapter.emit_end(wh::compose::graph_value{1}, context).has_value());
  REQUIRE(adapter.emit_error(wh::core::make_error(wh::core::errc::canceled),
                             context)
              .has_value());
}

TEST_CASE("compose callback adapter forwards configured hook payloads",
          "[UT][wh/compose/callback/adapter.hpp][callback_adapter::emit_end][condition][branch][boundary]") {
  wh::compose::callback_adapter adapter{};
  wh::compose::graph_value seen{};
  wh::core::error_code seen_error{};
  wh::core::run_context context{};

  adapter.on_start = [&](const wh::compose::graph_value &value,
                         wh::core::run_context &) -> wh::core::result<void> {
    seen = value;
    return {};
  };
  adapter.on_end = [&](const wh::compose::graph_value &value,
                       wh::core::run_context &) -> wh::core::result<void> {
    seen = value;
    return {};
  };
  adapter.on_error =
      [&](const wh::core::error_code error,
          wh::core::run_context &) -> wh::core::result<void> {
    seen_error = error;
    return {};
  };

  REQUIRE(adapter.emit_start(wh::compose::graph_value{7}, context).has_value());
  REQUIRE(*wh::core::any_cast<int>(&seen) == 7);
  REQUIRE(adapter.emit_end(wh::compose::graph_value{9}, context).has_value());
  REQUIRE(*wh::core::any_cast<int>(&seen) == 9);
  REQUIRE(adapter.emit_error(wh::core::make_error(wh::core::errc::canceled),
                             context)
              .has_value());
  REQUIRE(seen_error == wh::core::errc::canceled);
}

TEST_CASE("compose callback adapter propagates hook failures unchanged",
          "[UT][wh/compose/callback/adapter.hpp][callback_adapter::emit_error][condition][branch][boundary]") {
  wh::compose::callback_adapter adapter{};
  wh::core::run_context context{};

  adapter.on_end = [](const wh::compose::graph_value &,
                      wh::core::run_context &) -> wh::core::result<void> {
    return wh::core::result<void>::failure(wh::core::errc::timeout);
  };
  adapter.on_error =
      [](const wh::core::error_code,
         wh::core::run_context &) -> wh::core::result<void> {
    return wh::core::result<void>::failure(wh::core::errc::network_error);
  };

  REQUIRE(adapter.emit_end(wh::compose::graph_value{1}, context).error() ==
          wh::core::errc::timeout);
  REQUIRE(adapter.emit_error(wh::core::make_error(wh::core::errc::canceled),
                             context)
              .error() == wh::core::errc::network_error);
}
