#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "wh/schema/message/types.hpp"

TEST_CASE("message types normalize adjacent parts merge chunks and reject incompatible tool deltas",
          "[UT][wh/schema/message/types.hpp][normalize_message_parts][condition][branch]") {
  using wh::schema::audio_part;
  using wh::schema::message_part;
  using wh::schema::text_part;
  using wh::schema::tool_call_part;

  std::vector<message_part> parts{};
  parts.emplace_back(text_part{"he"});
  parts.emplace_back(text_part{"llo"});
  parts.emplace_back(audio_part{.base64 = "AA", .uri = ""});
  parts.emplace_back(audio_part{.base64 = "BB", .uri = ""});
  parts.emplace_back(
      tool_call_part{.index = 1, .id = "id", .name = "tool", .arguments = "{", .complete = false});
  parts.emplace_back(
      tool_call_part{.index = 1, .id = "id", .name = "tool", .arguments = "}", .complete = true});

  auto normalized = wh::schema::detail::normalize_message_parts(parts);
  REQUIRE(normalized.has_value());
  REQUIRE(normalized.value().size() == 3U);
  REQUIRE(std::get<text_part>(normalized.value()[0]).text == "hello");
  REQUIRE(std::get<audio_part>(normalized.value()[1]).base64 == "AABB");
  const auto &tool = std::get<tool_call_part>(normalized.value()[2]);
  REQUIRE(tool.arguments == "{}");
  REQUIRE_FALSE(tool.complete);

  std::vector<message_part> conflicting{};
  conflicting.emplace_back(tool_call_part{.index = 0, .id = "left", .name = "tool"});
  conflicting.emplace_back(tool_call_part{.index = 0, .id = "right", .name = "tool"});
  auto conflict = wh::schema::detail::normalize_message_parts(std::move(conflicting));
  REQUIRE(conflict.has_error());
  REQUIRE(conflict.error() == wh::core::errc::contract_violation);
}

TEST_CASE("message types merge chunk and update pipelines cover append insert replace reject and "
          "index acceleration",
          "[UT][wh/schema/message/types.hpp][apply_message_update][condition][branch][boundary]") {
  using wh::schema::message;
  using wh::schema::message_part;
  using wh::schema::message_role;
  using wh::schema::message_update_action;
  using wh::schema::message_update_mode;
  using wh::schema::text_part;

  message first{};
  first.message_id = "m1";
  first.role = message_role::assistant;
  first.parts.emplace_back(text_part{"he"});
  first.meta.usage.prompt_tokens = 2;

  message second{};
  second.message_id = "m1";
  second.role = message_role::assistant;
  second.parts.emplace_back(text_part{"llo"});
  second.meta.usage.prompt_tokens = 5;
  second.meta.finish_reason = "stop";

  auto merged = wh::schema::merge_message_chunks(std::vector<message>{first, second});
  REQUIRE(merged.has_value());
  REQUIRE(std::get<text_part>(merged.value().parts.front()).text == "hello");
  REQUIRE(merged.value().meta.usage.prompt_tokens == 5);
  REQUIRE(merged.value().meta.finish_reason == "stop");

  std::vector<message> messages{};
  std::vector<wh::schema::message_update_audit_entry> audit{};

  message append{};
  append.message_id = "a";
  append.role = message_role::assistant;
  append.parts.emplace_back(text_part{"v1"});
  REQUIRE(wh::schema::apply_message_update(messages, append, message_update_mode::append, &audit)
              .has_value());
  REQUIRE(messages.size() == 1U);
  REQUIRE(audit.back().action == message_update_action::appended);

  message inserted{};
  inserted.message_id = "b";
  inserted.role = message_role::assistant;
  inserted.parts.emplace_back(text_part{"v2"});
  REQUIRE(wh::schema::apply_message_update(messages, inserted, message_update_mode::upsert_by_id,
                                           &audit)
              .has_value());
  REQUIRE(messages.size() == 2U);
  REQUIRE(audit.back().action == message_update_action::inserted);

  message replaced = inserted;
  replaced.parts.clear();
  replaced.parts.emplace_back(text_part{"v3"});
  wh::schema::message_index index{};
  REQUIRE(wh::schema::apply_message_update(messages, replaced, index,
                                           message_update_mode::upsert_by_id, &audit)
              .has_value());
  REQUIRE(std::get<text_part>(messages[1].parts.front()).text == "v3");
  REQUIRE(audit.back().action == message_update_action::replaced);

  message missing_id{};
  missing_id.role = message_role::assistant;
  missing_id.parts.emplace_back(text_part{"x"});
  auto missing_id_status = wh::schema::apply_message_update(
      messages, missing_id, index, message_update_mode::upsert_by_id, &audit);
  REQUIRE(missing_id_status.has_error());
  REQUIRE(missing_id_status.error() == wh::core::errc::invalid_argument);
  REQUIRE(audit.back().action == message_update_action::rejected);

  message wrong_role = replaced;
  wrong_role.role = message_role::tool;
  auto wrong_role_status = wh::schema::apply_message_update(
      messages, wrong_role, index, message_update_mode::upsert_by_id, &audit);
  REQUIRE(wrong_role_status.has_error());
  REQUIRE(wrong_role_status.error() == wh::core::errc::contract_violation);
}
