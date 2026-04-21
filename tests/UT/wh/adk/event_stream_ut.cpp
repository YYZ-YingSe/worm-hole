#include <stdexcept>
#include <string>
#include <variant>

#include <catch2/catch_test_macros.hpp>

#include "wh/adk/event_stream.hpp"

namespace {

[[nodiscard]] auto make_message_event(const std::string &text) -> wh::adk::agent_event {
  wh::schema::message message{};
  message.role = wh::schema::message_role::assistant;
  message.parts.emplace_back(wh::schema::text_part{text});
  return wh::adk::make_message_event(std::move(message));
}

} // namespace

TEST_CASE("make_agent_event_stream sends reads and closes message events",
          "[UT][wh/adk/event_stream.hpp][make_agent_event_stream][condition][branch][boundary]") {
  auto [writer, reader] = wh::adk::make_agent_event_stream();
  REQUIRE(wh::adk::send_agent_event(writer, make_message_event("hello")).has_value());

  auto next = wh::adk::read_agent_event_stream(reader);
  REQUIRE(next.has_value());
  REQUIRE(next.value().value.has_value());
  REQUIRE(std::holds_alternative<wh::adk::message_event>(next.value().value->payload));

  REQUIRE(wh::adk::close_agent_event_stream(writer).has_value());
  auto eof = wh::adk::read_agent_event_stream(reader);
  REQUIRE(eof.has_value());
  REQUIRE(eof.value().eof);
}

TEST_CASE(
    "try_read_agent_event_stream reports pending for live streams and eof for unbound readers",
    "[UT][wh/adk/event_stream.hpp][try_read_agent_event_stream][condition][branch][boundary]") {
  auto [writer, reader] = wh::adk::make_agent_event_stream();
  auto pending = wh::adk::try_read_agent_event_stream(reader);
  REQUIRE(std::holds_alternative<wh::schema::stream::stream_signal>(pending));
  REQUIRE(std::get<wh::schema::stream::stream_signal>(pending) ==
          wh::schema::stream::stream_pending);
  REQUIRE(wh::adk::close_agent_event_stream(writer).has_value());
  REQUIRE(wh::adk::close_agent_event_stream(reader).has_value());

  wh::adk::agent_event_stream_reader empty_reader{};
  REQUIRE(wh::adk::read_agent_event_stream(empty_reader).value().eof);
  auto try_empty = wh::adk::try_read_agent_event_stream(empty_reader);
  REQUIRE(std::holds_alternative<wh::adk::agent_event_stream_result>(try_empty));
  REQUIRE(std::get<wh::adk::agent_event_stream_result>(try_empty).value().eof);
  REQUIRE(wh::adk::close_agent_event_stream(empty_reader).has_value());
}

TEST_CASE("send_agent_event maps closed and saturated writer failures to public errors",
          "[UT][wh/adk/event_stream.hpp][send_agent_event][condition][branch][boundary]") {
  wh::adk::agent_event_stream_writer empty_writer{};
  auto empty_send = wh::adk::send_agent_event(empty_writer, make_message_event("unused"));
  REQUIRE(empty_send.has_error());
  REQUIRE(empty_send.error() == wh::core::errc::channel_closed);
  REQUIRE(wh::adk::close_agent_event_stream(empty_writer).has_value());

  auto [raw_writer, raw_reader] = wh::schema::stream::make_pipe_stream<wh::adk::agent_event>(1U);
  wh::adk::agent_event_stream_writer small_writer{std::move(raw_writer)};
  wh::adk::agent_event_stream_reader small_reader{std::move(raw_reader)};
  REQUIRE(wh::adk::send_agent_event(small_writer, make_message_event("a")).has_value());
  auto full = wh::adk::send_agent_event(small_writer, make_message_event("b"));
  REQUIRE(full.has_error());
  REQUIRE(full.error() == wh::core::errc::resource_exhausted);
  REQUIRE(wh::adk::close_agent_event_stream(small_writer).has_value());
  REQUIRE(wh::adk::close_agent_event_stream(small_reader).has_value());
}

TEST_CASE("send_agent_event_or_error forwards successful factories unchanged",
          "[UT][wh/adk/event_stream.hpp][send_agent_event_or_error][condition][branch][boundary]") {
  auto [writer, reader] = wh::adk::make_agent_event_stream();
  REQUIRE(wh::adk::send_agent_event_or_error(writer, []() -> wh::adk::agent_event {
            wh::adk::event_metadata metadata{};
            metadata.agent_name = "factory";
            return wh::adk::make_message_event(
                wh::schema::message{.role = wh::schema::message_role::assistant,
                                    .parts = {wh::schema::text_part{"ok"}}},
                std::move(metadata));
          }).has_value());

  auto success = wh::adk::read_agent_event_stream(reader);
  REQUIRE(success.has_value());
  REQUIRE(success.value().value.has_value());
  auto *payload = std::get_if<wh::adk::message_event>(&success.value().value->payload);
  REQUIRE(payload != nullptr);
  REQUIRE(success.value().value->metadata.agent_name == "factory");
}

TEST_CASE("send_agent_event_or_error converts thrown failures into structured error events",
          "[UT][wh/adk/event_stream.hpp][send_agent_event_or_error][condition][branch]") {
  auto [error_writer, error_reader] = wh::adk::make_agent_event_stream();
  wh::adk::event_metadata metadata{};
  metadata.agent_name = "bridge";
  REQUIRE(wh::adk::send_agent_event_or_error(
              error_writer, []() -> wh::adk::agent_event { throw std::runtime_error{"boom"}; },
              metadata)
              .has_value());
  auto error_event = wh::adk::read_agent_event_stream(error_reader);
  REQUIRE(error_event.has_value());
  REQUIRE(error_event.value().value.has_value());
  auto *error_payload = std::get_if<wh::adk::error_event>(&error_event.value().value->payload);
  REQUIRE(error_payload != nullptr);
  REQUIRE(error_payload->code == wh::core::errc::internal_error);
  REQUIRE(error_payload->message == "boom");
  REQUIRE(error_event.value().value->metadata.agent_name == "bridge");

  auto [unknown_writer, unknown_reader] = wh::adk::make_agent_event_stream();
  REQUIRE(wh::adk::send_agent_event_or_error(
              unknown_writer, []() -> wh::adk::agent_event { throw 42; },
              wh::adk::event_metadata{.agent_name = "unknown"})
              .has_value());
  auto unknown_event = wh::adk::read_agent_event_stream(unknown_reader);
  REQUIRE(unknown_event.has_value());
  REQUIRE(unknown_event.value().value.has_value());
  auto *unknown_payload = std::get_if<wh::adk::error_event>(&unknown_event.value().value->payload);
  REQUIRE(unknown_payload != nullptr);
  REQUIRE(unknown_payload->code == wh::core::errc::internal_error);
  REQUIRE(unknown_payload->message == "unknown");
  REQUIRE(unknown_event.value().value->metadata.agent_name == "unknown");
}
