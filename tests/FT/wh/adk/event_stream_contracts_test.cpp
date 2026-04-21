#include <chrono>
#include <concepts>
#include <stdexcept>
#include <string>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "helper/sender_capture.hpp"
#include "wh/adk/detail/event_message_stream_reader.hpp"
#include "wh/adk/event_stream.hpp"
#include "wh/schema/stream.hpp"

namespace {

[[nodiscard]] auto make_message_event(const std::string &text) -> wh::adk::agent_event {
  wh::schema::message message{};
  message.role = wh::schema::message_role::assistant;
  message.parts.emplace_back(wh::schema::text_part{text});
  return wh::adk::make_message_event(std::move(message));
}

[[nodiscard]] auto make_assistant_message(const std::string &text) -> wh::schema::message {
  wh::schema::message message{};
  message.role = wh::schema::message_role::assistant;
  message.parts.emplace_back(wh::schema::text_part{text});
  return message;
}

} // namespace

TEST_CASE("adk event stream transports events and closes to eof", "[core][adk][condition]") {
  static_assert(!std::copy_constructible<wh::adk::agent_event>);
  static_assert(!std::copy_constructible<wh::adk::agent_event_stream_reader>);
  static_assert(requires {
    std::declval<wh::schema::stream::pipe_stream_reader<wh::adk::agent_event> &>().read_async();
    std::declval<wh::schema::stream::pipe_stream_writer<wh::adk::agent_event> &>().try_write(
        std::declval<wh::adk::agent_event &&>());
    std::declval<const wh::schema::stream::pipe_stream_writer<wh::adk::agent_event> &>()
        .write_async(std::declval<wh::adk::agent_event &&>());
  });

  auto [writer, reader] = wh::adk::make_agent_event_stream();
  REQUIRE(wh::adk::send_agent_event(writer, make_message_event("hello")).has_value());

  auto next = wh::adk::read_agent_event_stream(reader);
  REQUIRE(next.has_value());
  REQUIRE_FALSE(next.value().eof);
  REQUIRE(next.value().value.has_value());

  const auto *message = std::get_if<wh::adk::message_event>(&next.value().value->payload);
  REQUIRE(message != nullptr);
  REQUIRE(std::holds_alternative<wh::schema::message>(message->content));
  REQUIRE(std::get<wh::schema::message>(message->content).role ==
          wh::schema::message_role::assistant);

  REQUIRE(wh::adk::close_agent_event_stream(writer).has_value());
  auto eof = wh::adk::read_agent_event_stream(reader);
  REQUIRE(eof.has_value());
  REQUIRE(eof.value().eof);
}

TEST_CASE("adk event stream helpers normalize empty endpoints", "[core][adk][boundary]") {
  wh::adk::agent_event_stream_reader empty_reader{};
  auto empty_next = wh::adk::read_agent_event_stream(empty_reader);
  REQUIRE(empty_next.has_value());
  REQUIRE(empty_next.value().eof);
  REQUIRE(wh::adk::close_agent_event_stream(empty_reader).has_value());

  wh::adk::agent_event_stream_writer empty_writer{};
  REQUIRE(wh::adk::send_agent_event(empty_writer, make_message_event("unused")).has_error());
  REQUIRE(wh::adk::send_agent_event(empty_writer, make_message_event("unused")).error() ==
          wh::core::errc::channel_closed);
  REQUIRE(wh::adk::close_agent_event_stream(empty_writer).has_value());
}

TEST_CASE("adk event stream helpers convert producer exceptions into error events",
          "[core][adk][condition]") {
  auto [writer, reader] = wh::adk::make_agent_event_stream();
  REQUIRE(wh::adk::send_agent_event_or_error(writer, []() -> wh::adk::agent_event {
            throw std::runtime_error{"boom"};
          }).has_value());

  auto next = wh::adk::read_agent_event_stream(reader);
  REQUIRE(next.has_value());
  REQUIRE(next.value().value.has_value());

  const auto *error = std::get_if<wh::adk::error_event>(&next.value().value->payload);
  REQUIRE(error != nullptr);
  REQUIRE(error->code == wh::core::errc::internal_error);
  REQUIRE(error->message == "boom");
}

TEST_CASE("adk event-message stream async read forwards stop into nested "
          "message stream and preserves state",
          "[core][adk][stop][condition]") {
  auto [message_writer, message_reader] =
      wh::schema::stream::make_pipe_stream<wh::schema::message>(4U);

  std::vector<wh::adk::agent_event> events{};
  events.push_back(
      wh::adk::make_message_event(wh::adk::agent_message_stream_reader{std::move(message_reader)}));

  wh::adk::detail::event_message_stream_reader flattened{wh::adk::agent_event_stream_reader{
      wh::schema::stream::make_values_stream_reader(std::move(events))}};

  stdexec::inplace_stop_source stop_source{};
  wh::testing::helper::sender_capture<> completion{};
  auto operation = stdexec::connect(flattened.read_async(),
                                    wh::testing::helper::sender_capture_receiver{
                                        &completion,
                                        wh::testing::helper::make_scheduler_env(
                                            stdexec::inline_scheduler{}, stop_source.get_token()),
                                    });
  stdexec::start(operation);
  stop_source.request_stop();

  REQUIRE(completion.ready.try_acquire_for(std::chrono::milliseconds(100)));
  REQUIRE(completion.terminal == wh::testing::helper::sender_terminal_kind::stopped);

  REQUIRE(message_writer.try_write(make_assistant_message("resumed message")).has_value());
  REQUIRE(message_writer.close().has_value());

  auto resumed = flattened.read();
  REQUIRE(resumed.has_value());
  REQUIRE_FALSE(resumed.value().eof);
  REQUIRE_FALSE(resumed.value().error.failed());
  REQUIRE(resumed.value().value.has_value());
  REQUIRE(std::get<wh::schema::text_part>(resumed.value().value->parts.front()).text ==
          "resumed message");

  auto eof = flattened.read();
  REQUIRE(eof.has_value());
  REQUIRE(eof.value().eof);
}
