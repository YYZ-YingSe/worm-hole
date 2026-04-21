#include <cstdint>
#include <string>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "wh/adk/types.hpp"
#include "wh/schema/stream/pipe.hpp"

static_assert(std::same_as<wh::adk::run_path, wh::core::address>);

TEST_CASE("adk types make_message_event preserves message payload and metadata",
          "[UT][wh/adk/types.hpp][make_message_event][condition][branch][boundary]") {
  wh::schema::message message{};
  message.role = wh::schema::message_role::assistant;
  auto event =
      wh::adk::make_message_event(message, wh::adk::event_metadata{.agent_name = "planner"});

  REQUIRE(std::holds_alternative<wh::adk::message_event>(event.payload));
  REQUIRE(event.metadata.agent_name == "planner");
  const auto *payload = std::get_if<wh::adk::message_event>(&event.payload);
  REQUIRE(payload != nullptr);
  REQUIRE(std::holds_alternative<wh::schema::message>(payload->content));
}

TEST_CASE("adk types make_message_event supports message-stream payloads and rejects them for "
          "checkpointing",
          "[UT][wh/adk/"
          "types.hpp][validate_agent_event_checkpoint_serializable][condition][branch][boundary]") {
  auto [writer, reader] = wh::schema::stream::make_pipe_stream<wh::schema::message>(1U);
  auto streamed =
      wh::adk::make_message_event(wh::adk::agent_message_stream_reader{std::move(reader)});
  wh::schema::serialization_registry registry{};

  REQUIRE(std::holds_alternative<wh::adk::message_event>(streamed.payload));
  auto streamed_status = wh::adk::validate_agent_event_checkpoint_serializable(streamed, registry);
  REQUIRE(streamed_status.has_error());
  REQUIRE(streamed_status.error() == wh::core::errc::serialize_error);
  static_cast<void>(writer);
}

TEST_CASE("adk types build control custom and error events with owned payloads",
          "[UT][wh/adk/types.hpp][make_error_event][condition][branch][boundary]") {
  auto control = wh::adk::make_control_event(
      {.kind = wh::adk::control_action_kind::transfer, .target = "worker"});
  auto *action = std::get_if<wh::adk::control_action>(&control.payload);
  REQUIRE(action != nullptr);
  REQUIRE(action->kind == wh::adk::control_action_kind::transfer);
  REQUIRE(action->target == "worker");

  auto custom = wh::adk::make_custom_event("metric", wh::core::any{9}, {.tool_name = "search"});
  auto *custom_payload = std::get_if<wh::adk::custom_event>(&custom.payload);
  REQUIRE(custom_payload != nullptr);
  REQUIRE(custom_payload->name == "metric");
  REQUIRE(*wh::core::any_cast<int>(&custom_payload->payload) == 9);
  REQUIRE(custom.metadata.tool_name == "search");

  auto error = wh::adk::make_error_event(wh::core::errc::invalid_argument, "bad",
                                         wh::core::any{std::string{"detail"}});
  auto *error_payload = std::get_if<wh::adk::error_event>(&error.payload);
  REQUIRE(error_payload != nullptr);
  REQUIRE(error_payload->code == wh::core::errc::invalid_argument);
  REQUIRE(error_payload->message == "bad");
  REQUIRE(*wh::core::any_cast<std::string>(&error_payload->detail) == "detail");
}

TEST_CASE(
    "adk types validate checkpoint-serializable payload registration for custom and error events",
    "[UT][wh/adk/"
    "types.hpp][validate_agent_event_checkpoint_serializable][condition][branch][boundary]") {
  wh::schema::message message{};
  auto message_event = wh::adk::make_message_event(message);

  auto custom = wh::adk::make_custom_event("metric", wh::core::any{9});
  auto empty_custom = wh::adk::make_custom_event("metric", wh::core::any{});
  auto error = wh::adk::make_error_event(wh::core::errc::invalid_argument, "bad",
                                         wh::core::any{std::string{"detail"}});

  wh::schema::serialization_registry registry{};
  REQUIRE(registry.register_type<std::int64_t>("my.i64").has_value());
  REQUIRE(registry.register_type<std::string>("my.string").has_value());

  REQUIRE(
      wh::adk::validate_agent_event_checkpoint_serializable(message_event, registry).has_value());
  REQUIRE(wh::adk::validate_agent_event_checkpoint_serializable(custom, registry).has_error());
  REQUIRE(
      wh::adk::validate_agent_event_checkpoint_serializable(empty_custom, registry).has_value());
  REQUIRE(wh::adk::validate_agent_event_checkpoint_serializable(error, registry).has_value());
}
