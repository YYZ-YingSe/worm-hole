#include <catch2/catch_test_macros.hpp>

#include "helper/agent_authoring_support.hpp"
#include "wh/adk/detail/history_request.hpp"

TEST_CASE(
    "history request helpers detect trailing tool calls rewrite react history and read payload "
    "views",
    "[UT][wh/adk/detail/history_request.hpp][make_history_request][condition][branch][boundary]") {
  wh::schema::message assistant_with_call{};
  assistant_with_call.role = wh::schema::message_role::assistant;
  assistant_with_call.parts.emplace_back(wh::schema::tool_call_part{
      .index = 0U,
      .id = "call-1",
      .type = "function",
      .name = "search",
      .arguments = "{\"q\":\"hi\"}",
      .complete = true,
  });
  REQUIRE(wh::adk::detail::history_request_has_tool_call_part(assistant_with_call));
  REQUIRE_FALSE(wh::adk::detail::history_request_has_tool_call_part(
      wh::testing::helper::make_text_message(wh::schema::message_role::assistant, "draft")));

  wh::agent::react_state state{};
  state.messages.push_back(
      wh::testing::helper::make_text_message(wh::schema::message_role::system, "system"));
  state.messages.push_back(
      wh::testing::helper::make_text_message(wh::schema::message_role::user, "question"));
  state.messages.push_back(
      wh::testing::helper::make_text_message(wh::schema::message_role::tool, "tool result"));
  state.messages.push_back(assistant_with_call);
  auto history_request = wh::adk::detail::make_history_request(state);
  REQUIRE(history_request.has_value());
  REQUIRE(history_request.value().messages.size() == 2U);
  REQUIRE(history_request.value().messages.front().role == wh::schema::message_role::user);
  REQUIRE(std::get<wh::schema::text_part>(history_request.value().messages.front().parts.front())
              .text == "question");
  REQUIRE(std::get<wh::schema::text_part>(history_request.value().messages.back().parts.front())
              .text.find("For context: tool returned result: tool result.") != std::string::npos);

  wh::agent::react_state empty_state{};
  empty_state.messages.push_back(
      wh::testing::helper::make_text_message(wh::schema::message_role::system, "system"));
  auto empty_history_request = wh::adk::detail::make_history_request(empty_state);
  REQUIRE(empty_history_request.has_error());
  REQUIRE(empty_history_request.error() == wh::core::errc::not_found);

  wh::model::chat_request request{};
  request.messages.push_back(
      wh::testing::helper::make_text_message(wh::schema::message_role::user, "payload"));
  wh::core::any request_any{request};
  auto view_from_request = wh::adk::detail::read_history_request_payload_view(request_any);
  REQUIRE(view_from_request.has_value());
  REQUIRE(view_from_request.value().history_request != nullptr);
  REQUIRE(view_from_request.value().state_payload == nullptr);

  wh::adk::detail::history_request_payload payload{};
  payload.history_request = request;
  payload.state_payload = wh::core::any{std::string{"state"}};
  wh::core::any payload_any{payload};
  auto view_from_payload = wh::adk::detail::read_history_request_payload_view(payload_any);
  REQUIRE(view_from_payload.has_value());
  REQUIRE(view_from_payload.value().history_request != nullptr);
  REQUIRE(view_from_payload.value().state_payload != nullptr);
  REQUIRE(*wh::core::any_cast<std::string>(view_from_payload.value().state_payload) == "state");

  auto copied_payload = wh::adk::detail::read_history_request_payload(payload_any);
  REQUIRE(copied_payload.has_value());
  REQUIRE(copied_payload.value().history_request.messages.size() == 1U);
  REQUIRE(*wh::core::any_cast<std::string>(&copied_payload.value().state_payload) == "state");

  auto missing = wh::adk::detail::read_history_request_payload_view(wh::core::any{});
  REQUIRE(missing.has_error());
  REQUIRE(missing.error() == wh::core::errc::not_found);
  auto wrong_type = wh::adk::detail::read_history_request_payload_view(wh::core::any{3});
  REQUIRE(wrong_type.has_error());
  REQUIRE(wrong_type.error() == wh::core::errc::type_mismatch);
}

TEST_CASE("history request keeps trailing assistant text when the tail has no tool call",
          "[UT][wh/adk/detail/"
          "history_request.hpp][history_request_has_tool_call_part][condition][branch][boundary]") {
  wh::agent::react_state state{};
  state.messages.push_back(
      wh::testing::helper::make_text_message(wh::schema::message_role::system, "system"));
  state.messages.push_back(
      wh::testing::helper::make_text_message(wh::schema::message_role::user, "question"));
  state.messages.push_back(
      wh::testing::helper::make_text_message(wh::schema::message_role::assistant, "answer"));

  auto request = wh::adk::detail::make_history_request(state);
  REQUIRE(request.has_value());
  REQUIRE(request.value().messages.size() == 2U);
  REQUIRE(request.value().messages.back().role == wh::schema::message_role::user);
  REQUIRE(std::get<wh::schema::text_part>(request.value().messages.back().parts.front())
              .text.find("assistant said: answer.") != std::string::npos);
}
