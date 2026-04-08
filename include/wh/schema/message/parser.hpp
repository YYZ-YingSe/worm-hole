// Defines parsing helpers that convert between message schema forms and
// textual/JSON representations used by tool-call and output flows.
#pragma once

#include <cstddef>
#include <ranges>
#include <string>
#include <string_view>

#include "wh/core/error.hpp"
#include "wh/core/json.hpp"
#include "wh/core/result.hpp"
#include "wh/schema/message/types.hpp"

namespace wh::schema {

/// Input extraction mode for parsing message JSON payload.
enum class message_parse_from {
  /// Treat whole input as raw text content.
  content,
  /// Parse full JSON object into canonical message fields.
  full_json,
  /// Parse first tool-call arguments payload as text.
  tool_calls,
};

/// Parsing configuration including extraction path and defaults.
struct message_parse_config {
  /// Parsing mode that selects input interpretation.
  message_parse_from from{message_parse_from::content};
  /// Dot-separated path used before parsing JSON object payload.
  std::string key_path{};
  /// Fallback role when role field is absent.
  message_role default_role{message_role::assistant};
  /// Fallback name when name field is absent.
  std::string default_name{};
};

namespace detail {

/// Parses role string into `message_role`.
[[nodiscard]] inline auto parse_role(const std::string_view role)
    -> wh::core::result<message_role> {
  if (role == "system") {
    return message_role::system;
  }
  if (role == "user") {
    return message_role::user;
  }
  if (role == "assistant") {
    return message_role::assistant;
  }
  if (role == "tool") {
    return message_role::tool;
  }
  return wh::core::result<message_role>::failure(wh::core::errc::parse_error);
}

/// Extracts nested JSON node using dot-separated key path.
[[nodiscard]] inline auto extract_by_key_path(const wh::core::json_value &root,
                                              const std::string_view path)
    -> wh::core::result<const wh::core::json_value *> {
  if (path.empty()) {
    return &root;
  }
  const wh::core::json_value *current = &root;
  std::size_t begin = 0U;
  while (begin <= path.size()) {
    const auto end = path.find('.', begin);
    const auto segment = end == std::string_view::npos
                             ? path.substr(begin)
                             : path.substr(begin, end - begin);
    auto member = wh::core::json_find_member(*current, segment);
    if (member.has_error()) {
      return wh::core::result<const wh::core::json_value *>::failure(
          wh::core::errc::not_found);
    }
    current = member.value();
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1U;
  }
  return current;
}

/// Parses canonical message fields from a JSON object node.
[[nodiscard]] inline auto
parse_message_from_json_value(const wh::core::json_value &input,
                              const message_parse_config &config)
    -> wh::core::result<message> {
  if (!input.IsObject()) {
    return wh::core::result<message>::failure(wh::core::errc::type_mismatch);
  }

  message parsed{};
  parsed.role = config.default_role;
  parsed.name = config.default_name;

  if (const auto role_member = input.FindMember("role");
      role_member != input.MemberEnd() && role_member->value.IsString()) {
    auto role = parse_role(std::string_view{
        role_member->value.GetString(), role_member->value.GetStringLength()});
    if (role.has_error()) {
      return wh::core::result<message>::failure(role.error());
    }
    parsed.role = role.value();
  }
  if (const auto name_member = input.FindMember("name");
      name_member != input.MemberEnd() && name_member->value.IsString()) {
    parsed.name = std::string{name_member->value.GetString(),
                              name_member->value.GetStringLength()};
  }
  if (const auto id_member = input.FindMember("id");
      id_member != input.MemberEnd() && id_member->value.IsString()) {
    parsed.message_id = std::string{id_member->value.GetString(),
                                    id_member->value.GetStringLength()};
  }

  if (const auto content_member = input.FindMember("content");
      content_member != input.MemberEnd() && content_member->value.IsString()) {
    parsed.parts.emplace_back(
        text_part{std::string{content_member->value.GetString(),
                              content_member->value.GetStringLength()}});
  }

  if (const auto tool_calls = input.FindMember("tool_calls");
      tool_calls != input.MemberEnd() && tool_calls->value.IsArray()) {
    std::size_t index = 0U;
    for (const auto &tool_call : tool_calls->value.GetArray()) {
      if (!tool_call.IsObject()) {
        continue;
      }
      tool_call_part call{};
      call.index = index++;

      if (const auto member = tool_call.FindMember("id");
          member != tool_call.MemberEnd() && member->value.IsString()) {
        call.id = std::string{member->value.GetString(),
                              member->value.GetStringLength()};
      }
      if (const auto member = tool_call.FindMember("type");
          member != tool_call.MemberEnd() && member->value.IsString()) {
        call.type = std::string{member->value.GetString(),
                                member->value.GetStringLength()};
      }

      if (const auto fn = tool_call.FindMember("function");
          fn != tool_call.MemberEnd() && fn->value.IsObject()) {
        if (const auto member = fn->value.FindMember("name");
            member != fn->value.MemberEnd() && member->value.IsString()) {
          call.name = std::string{member->value.GetString(),
                                  member->value.GetStringLength()};
        }
        if (const auto member = fn->value.FindMember("arguments");
            member != fn->value.MemberEnd() && member->value.IsString()) {
          call.arguments = std::string{member->value.GetString(),
                                       member->value.GetStringLength()};
        }
      }

      parsed.parts.emplace_back(std::move(call));
    }
  }

  if (parsed.parts.empty()) {
    return wh::core::result<message>::failure(wh::core::errc::not_found);
  }
  return parsed;
}

} // namespace detail

/// Parses text/JSON input into canonical `message`.
[[nodiscard]] inline auto parse_message(const std::string_view input,
                                        const message_parse_config &config = {})
    -> wh::core::result<message> {
  if (config.from == message_parse_from::content) {
    message parsed{};
    parsed.role = config.default_role;
    parsed.name = config.default_name;
    parsed.parts.emplace_back(text_part{std::string{input}});
    return parsed;
  }

  auto root = wh::core::parse_json(input);
  if (root.has_error()) {
    return wh::core::result<message>::failure(wh::core::errc::parse_error);
  }

  if (config.from == message_parse_from::full_json) {
    auto extracted = detail::extract_by_key_path(root.value(), config.key_path);
    if (extracted.has_error()) {
      return wh::core::result<message>::failure(extracted.error());
    }
    return detail::parse_message_from_json_value(*extracted.value(), config);
  }

  if (config.from == message_parse_from::tool_calls) {
    auto extracted = detail::extract_by_key_path(root.value(), config.key_path);
    if (extracted.has_error()) {
      return wh::core::result<message>::failure(extracted.error());
    }
    const auto *selected = extracted.value();
    if (!selected->IsObject()) {
      return wh::core::result<message>::failure(wh::core::errc::type_mismatch);
    }

    const auto tool_calls = selected->FindMember("tool_calls");
    if (tool_calls == selected->MemberEnd() || !tool_calls->value.IsArray() ||
        tool_calls->value.Empty()) {
      return wh::core::result<message>::failure(wh::core::errc::not_found);
    }

    const auto &first_call = tool_calls->value[0];
    if (!first_call.IsObject()) {
      return wh::core::result<message>::failure(wh::core::errc::type_mismatch);
    }
    const auto function = first_call.FindMember("function");
    if (function == first_call.MemberEnd() || !function->value.IsObject()) {
      return wh::core::result<message>::failure(wh::core::errc::not_found);
    }
    const auto arguments = function->value.FindMember("arguments");
    if (arguments == function->value.MemberEnd() ||
        !arguments->value.IsString()) {
      return wh::core::result<message>::failure(wh::core::errc::not_found);
    }

    message parsed{};
    parsed.role = config.default_role;
    parsed.name = config.default_name;
    parsed.parts.emplace_back(text_part{std::string{
        arguments->value.GetString(), arguments->value.GetStringLength()}});
    return parsed;
  }

  return wh::core::result<message>::failure(wh::core::errc::invalid_argument);
}

} // namespace wh::schema
