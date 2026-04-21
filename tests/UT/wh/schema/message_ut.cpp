#include <catch2/catch_test_macros.hpp>

#include "wh/schema/message.hpp"

TEST_CASE("message facade parses content and merges normalized chunks",
          "[UT][wh/schema/message.hpp][parse_message][condition][branch][boundary]") {
  const auto parsed =
      wh::schema::parse_message(R"({"role":"assistant","content":"hello"})",
                                {.from = wh::schema::message_parse_from::full_json});
  REQUIRE(parsed.has_value());
  REQUIRE(parsed.value().role == wh::schema::message_role::assistant);
  REQUIRE(std::holds_alternative<wh::schema::text_part>(parsed.value().parts.front()));

  wh::schema::message first{};
  first.role = wh::schema::message_role::assistant;
  first.parts.emplace_back(wh::schema::text_part{"he"});

  wh::schema::message second{};
  second.role = wh::schema::message_role::assistant;
  second.parts.emplace_back(wh::schema::text_part{"llo"});

  auto merged = wh::schema::merge_message_chunks(std::vector<wh::schema::message>{first, second});
  REQUIRE(merged.has_value());
  REQUIRE(std::get<wh::schema::text_part>(merged.value().parts.front()).text == "hello");
}

TEST_CASE("message facade surfaces parse failure on malformed json input",
          "[UT][wh/schema/message.hpp][parse_message][condition][branch][boundary][error]") {
  auto malformed = wh::schema::parse_message(R"({"role":"assistant","content":)",
                                             {.from = wh::schema::message_parse_from::full_json});
  REQUIRE(malformed.has_error());
}
