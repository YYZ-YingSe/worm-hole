#include <catch2/catch_test_macros.hpp>

#include "wh/adk/detail/live_event_bridge.hpp"

TEST_CASE("make_live_event_bridge returns connected writer and reader endpoints",
          "[UT][wh/adk/detail/live_event_bridge.hpp][make_live_event_bridge][boundary]") {
  auto bridge = wh::adk::detail::make_live_event_bridge();
  REQUIRE(bridge.close().has_value());
}

TEST_CASE("live_event_bridge emits events then yields eof after closing writer side",
          "[UT][wh/adk/detail/live_event_bridge.hpp][live_event_bridge::emit][condition][branch][boundary]") {
  auto bridge = wh::adk::detail::make_live_event_bridge();
  REQUIRE(bridge.emit(wh::adk::make_control_event(
                          wh::adk::control_action{
                              .kind = wh::adk::control_action_kind::exit,
                          }))
              .has_value());
  REQUIRE(bridge.close().has_value());

  auto reader = bridge.release_reader();
  auto next = wh::adk::read_agent_event_stream(reader);
  REQUIRE(next.has_value());
  REQUIRE(next.value().value.has_value());
  REQUIRE(std::holds_alternative<wh::adk::control_action>(
      next.value().value->payload));

  auto eof = wh::adk::read_agent_event_stream(reader);
  REQUIRE(eof.has_value());
  REQUIRE(eof.value().eof);
}

TEST_CASE("live_event_bridge close prevents later emits from succeeding",
          "[UT][wh/adk/detail/live_event_bridge.hpp][live_event_bridge::close][branch][boundary]") {
  auto bridge = wh::adk::detail::make_live_event_bridge();
  REQUIRE(bridge.close().has_value());

  auto status = bridge.emit(wh::adk::make_control_event(
      wh::adk::control_action{.kind = wh::adk::control_action_kind::exit}));
  REQUIRE(status.has_error());
  REQUIRE(status.error() == wh::core::errc::channel_closed);
}
