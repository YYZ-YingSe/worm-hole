#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <utility>

#include "wh/adk/interrupt.hpp"
#include "wh/adk/types.hpp"
#include "wh/schema/serialization/registry.hpp"
#include "wh/schema/stream/pipe.hpp"

namespace {

[[nodiscard]] auto make_message(const std::string &text) -> wh::schema::message {
  wh::schema::message message{};
  message.role = wh::schema::message_role::assistant;
  message.parts.emplace_back(wh::schema::text_part{text});
  return message;
}

struct unregistered_payload {
  int value{0};
};

} // namespace

TEST_CASE("adk event payload and run path contracts are stable",
          "[core][adk][condition]") {
  const wh::adk::run_path path{{"root", "planner", "worker"}};

  auto message_event = wh::adk::make_message_event(
      make_message("hello"),
      wh::adk::event_metadata{
          .run_path = path,
          .agent_name = "planner",
          .tool_name = "search",
      });

  REQUIRE(message_event.metadata.run_path == path);
  REQUIRE(message_event.metadata.agent_name == "planner");
  REQUIRE(message_event.metadata.tool_name == "search");
  REQUIRE(std::holds_alternative<wh::adk::message_event>(message_event.payload));

  const auto &payload = std::get<wh::adk::message_event>(message_event.payload);
  REQUIRE(std::holds_alternative<wh::schema::message>(payload.content));
  REQUIRE(std::get<wh::schema::message>(payload.content).role ==
          wh::schema::message_role::assistant);

  auto control_event = wh::adk::make_control_event(
      wh::adk::control_action{
          .kind = wh::adk::control_action_kind::transfer,
          .target = "delegate",
      },
      wh::adk::event_metadata{.run_path = path});
  REQUIRE(std::holds_alternative<wh::adk::control_action>(control_event.payload));
  REQUIRE(std::get<wh::adk::control_action>(control_event.payload).target ==
          "delegate");
}

TEST_CASE("adk interrupt patch keeps payload naming aligned with event payload semantics",
          "[core][adk][condition]") {
  wh::adk::interrupt_patch patch{};
  patch.resolution = wh::adk::interrupt_resolution::edit;
  patch.payload = wh::core::any{42};
  patch.audit.actor = "tester";

  const auto *typed = wh::core::any_cast<int>(&patch.payload);
  REQUIRE(typed != nullptr);
  REQUIRE(*typed == 42);
  REQUIRE(patch.audit.actor == "tester");
}

TEST_CASE("adk checkpoint serialization validation rejects raw streams and unregistered payloads",
          "[core][adk][boundary]") {
  wh::schema::serialization_registry registry{};
  REQUIRE(registry.register_type_with_diagnostic_alias<std::int64_t>().has_value());
  registry.freeze();

  auto registered_error = wh::adk::make_error_event(
      wh::core::make_error(wh::core::errc::internal_error), "registered",
      wh::core::any{std::int64_t{7}});
  REQUIRE(wh::adk::validate_agent_event_checkpoint_serializable(registered_error,
                                                                registry)
              .has_value());

  auto unregistered_error = wh::adk::make_error_event(
      wh::core::make_error(wh::core::errc::internal_error), "unregistered",
      wh::core::any{std::in_place_type<unregistered_payload>,
                    unregistered_payload{.value = 9}});
  REQUIRE(wh::adk::validate_agent_event_checkpoint_serializable(unregistered_error,
                                                                registry)
              .has_error());
  REQUIRE(wh::adk::validate_agent_event_checkpoint_serializable(unregistered_error,
                                                                registry)
              .error() == wh::core::errc::serialize_error);

  auto [writer, reader] =
      wh::schema::stream::make_pipe_stream<wh::schema::message>(2U);
  auto stream_event = wh::adk::make_message_event(
      wh::adk::agent_message_stream_reader{std::move(reader)});
  REQUIRE(wh::adk::validate_agent_event_checkpoint_serializable(stream_event,
                                                                registry)
              .has_error());
  REQUIRE(wh::adk::validate_agent_event_checkpoint_serializable(stream_event,
                                                                registry)
              .error() == wh::core::errc::serialize_error);
  REQUIRE(writer.close().has_value());
}
