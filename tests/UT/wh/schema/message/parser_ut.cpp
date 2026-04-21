#include <catch2/catch_test_macros.hpp>

#include "wh/core/json.hpp"
#include "wh/schema/message/parser.hpp"

TEST_CASE(
    "message parser detail helpers cover role parsing key-path extraction and json object decoding",
    "[UT][wh/schema/message/parser.hpp][parse_message][condition][branch][boundary]") {
  auto assistant = wh::schema::detail::parse_role("assistant");
  auto invalid_role = wh::schema::detail::parse_role("unknown");
  REQUIRE(assistant.has_value());
  REQUIRE(assistant.value() == wh::schema::message_role::assistant);
  REQUIRE(invalid_role.has_error());
  REQUIRE(invalid_role.error() == wh::core::errc::parse_error);

  auto object_json = wh::core::parse_json(
      R"({"role":"assistant","content":"json-content","tool_calls":[{"id":"c1","function":{"name":"sum","arguments":"{\"k\":1}"}}]})");
  REQUIRE(object_json.has_value());
  auto parsed_message = wh::schema::detail::parse_message_from_json_value(
      object_json.value(), {.from = wh::schema::message_parse_from::full_json,
                            .default_role = wh::schema::message_role::user,
                            .default_name = "fallback"});
  REQUIRE(parsed_message.has_value());
  REQUIRE(parsed_message.value().role == wh::schema::message_role::assistant);
  REQUIRE(parsed_message.value().parts.size() == 2U);
  REQUIRE(std::holds_alternative<wh::schema::text_part>(parsed_message.value().parts.front()));
}

TEST_CASE("message parser covers content full-json tool-call and invalid branches",
          "[UT][wh/schema/message/parser.hpp][parse_message][condition][branch][boundary]") {
  auto plain = wh::schema::parse_message("hello", {.from = wh::schema::message_parse_from::content,
                                                   .default_role = wh::schema::message_role::tool,
                                                   .default_name = "plain"});
  REQUIRE(plain.has_value());
  REQUIRE(plain.value().role == wh::schema::message_role::tool);
  REQUIRE(plain.value().name == "plain");
  REQUIRE(std::get<wh::schema::text_part>(plain.value().parts.front()).text == "hello");

  auto full_json = wh::schema::parse_message(
      R"({"message":{"role":"assistant","content":"json-content"}})",
      {.from = wh::schema::message_parse_from::full_json, .key_path = "message"});
  REQUIRE(full_json.has_value());
  REQUIRE(std::get<wh::schema::text_part>(full_json.value().parts.front()).text == "json-content");

  auto missing_path = wh::schema::parse_message(
      R"({"message":{"role":"assistant","content":"json-content"}})",
      {.from = wh::schema::message_parse_from::full_json, .key_path = "missing"});
  REQUIRE(missing_path.has_error());
  REQUIRE(missing_path.error() == wh::core::errc::not_found);

  auto tool_calls =
      wh::schema::parse_message(R"({"tool_calls":[{"function":{"arguments":"{\"k\":1}"}}]})",
                                {.from = wh::schema::message_parse_from::tool_calls});
  REQUIRE(tool_calls.has_value());
  REQUIRE(std::get<wh::schema::text_part>(tool_calls.value().parts.front()).text == R"({"k":1})");

  auto missing_tool_calls = wh::schema::parse_message(
      R"({"tool_calls":[]})", {.from = wh::schema::message_parse_from::tool_calls});
  REQUIRE(missing_tool_calls.has_error());
  REQUIRE(missing_tool_calls.error() == wh::core::errc::not_found);

  auto bad_json =
      wh::schema::parse_message("{", {.from = wh::schema::message_parse_from::full_json});
  REQUIRE(bad_json.has_error());
  REQUIRE(bad_json.error() == wh::core::errc::parse_error);
}
