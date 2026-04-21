#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "helper/sender_capture.hpp"
#include "wh/adk/detail/event_message_stream_reader.hpp"
#include "wh/schema/stream/reader/values_stream_reader.hpp"

namespace {

[[nodiscard]] auto make_assistant_message(const std::string &text) -> wh::schema::message {
  wh::schema::message message{};
  message.role = wh::schema::message_role::assistant;
  message.parts.emplace_back(wh::schema::text_part{text});
  return message;
}

} // namespace

TEST_CASE("event message stream detail helper consumes direct messages message streams and visitor "
          "failures",
          "[UT][wh/adk/detail/"
          "event_message_stream_reader.hpp][consume_message_event_messages][condition][branch]["
          "boundary]") {
  std::vector<std::string> seen{};
  auto direct = wh::adk::detail::consume_message_event_messages(
      wh::adk::message_event{.content = make_assistant_message("one")},
      [&seen](wh::schema::message message) -> wh::core::result<void> {
        seen.push_back(std::get<wh::schema::text_part>(message.parts.front()).text);
        return {};
      });
  REQUIRE(direct.has_value());
  REQUIRE(seen == std::vector<std::string>{"one"});

  wh::adk::agent_message_stream_reader reader{wh::schema::stream::make_values_stream_reader(
      std::vector<wh::schema::message>{make_assistant_message("a"), make_assistant_message("b")})};
  auto stream = wh::adk::detail::consume_message_event_messages(
      wh::adk::message_event{.content = std::move(reader)},
      [&seen](wh::schema::message message) -> wh::core::result<void> {
        seen.push_back(std::get<wh::schema::text_part>(message.parts.front()).text);
        return {};
      });
  REQUIRE(stream.has_value());
  REQUIRE(seen == std::vector<std::string>{"one", "a", "b"});

  wh::adk::agent_message_stream_reader failing_reader{wh::schema::stream::make_values_stream_reader(
      std::vector<wh::schema::message>{make_assistant_message("x")})};
  auto visitor_error = wh::adk::detail::consume_message_event_messages(
      wh::adk::message_event{.content = std::move(failing_reader)},
      [](wh::schema::message) -> wh::core::result<void> {
        return wh::core::result<void>::failure(wh::core::errc::unavailable);
      });
  REQUIRE(visitor_error.has_error());
  REQUIRE(visitor_error.error() == wh::core::errc::unavailable);
}

TEST_CASE(
    "event message stream reader flattens message events skips controls and maps errors and eof",
    "[UT][wh/adk/detail/"
    "event_message_stream_reader.hpp][event_message_stream_reader::read_impl][condition][branch]["
    "boundary]") {
  std::vector<wh::schema::message> nested_messages{
      make_assistant_message("nested-a"),
      make_assistant_message("nested-b"),
  };
  std::vector<wh::adk::agent_event> events{};
  events.push_back(wh::adk::make_control_event(
      wh::adk::control_action{.kind = wh::adk::control_action_kind::transfer}));
  events.push_back(wh::adk::make_message_event(make_assistant_message("direct")));
  events.push_back(wh::adk::make_message_event(wh::adk::agent_message_stream_reader{
      wh::schema::stream::make_values_stream_reader(std::move(nested_messages))}));
  events.push_back(
      wh::adk::make_error_event(wh::core::make_error(wh::core::errc::unavailable), "boom"));

  wh::adk::detail::event_message_stream_reader flattened{wh::adk::agent_event_stream_reader{
      wh::schema::stream::make_values_stream_reader(std::move(events))}};

  auto first = flattened.read();
  REQUIRE(first.has_value());
  REQUIRE(first.value().value.has_value());
  REQUIRE(std::get<wh::schema::text_part>(first.value().value->parts.front()).text == "direct");

  auto second = flattened.read();
  REQUIRE(second.has_value());
  REQUIRE(second.value().value.has_value());
  REQUIRE(std::get<wh::schema::text_part>(second.value().value->parts.front()).text == "nested-a");

  auto third = flattened.read();
  REQUIRE(third.has_value());
  REQUIRE(third.value().value.has_value());
  REQUIRE(std::get<wh::schema::text_part>(third.value().value->parts.front()).text == "nested-b");

  auto error = flattened.read();
  REQUIRE(error.has_value());
  REQUIRE(error.value().error == wh::core::errc::unavailable);

  auto eof = flattened.read();
  REQUIRE(eof.has_value());
  REQUIRE(eof.value().eof);
  REQUIRE(flattened.is_closed());
}

TEST_CASE("event message stream reader try_read async stop and close preserve nested reader state",
          "[UT][wh/adk/detail/"
          "event_message_stream_reader.hpp][event_message_stream_reader::read_async][condition]["
          "branch][boundary][concurrency]") {
  auto [message_writer, message_reader] =
      wh::schema::stream::make_pipe_stream<wh::schema::message>(4U);

  std::vector<wh::adk::agent_event> events{};
  events.push_back(
      wh::adk::make_message_event(wh::adk::agent_message_stream_reader{std::move(message_reader)}));

  wh::adk::detail::event_message_stream_reader flattened{wh::adk::agent_event_stream_reader{
      wh::schema::stream::make_values_stream_reader(std::move(events))}};

  auto pending = flattened.try_read();
  REQUIRE(std::holds_alternative<wh::schema::stream::stream_signal>(pending));
  REQUIRE(std::get<wh::schema::stream::stream_signal>(pending) ==
          wh::schema::stream::stream_pending);

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

  REQUIRE(message_writer.try_write(make_assistant_message("resumed")).has_value());
  auto resumed = flattened.read();
  REQUIRE(resumed.has_value());
  REQUIRE(resumed.value().value.has_value());
  REQUIRE(std::get<wh::schema::text_part>(resumed.value().value->parts.front()).text == "resumed");

  REQUIRE(flattened.close().has_value());
  REQUIRE(flattened.is_closed());
}
