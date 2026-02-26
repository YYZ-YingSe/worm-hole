#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"

namespace wh::schema {

enum class message_role { system, user, assistant, tool };

struct text_part {
  std::string text{};
};

struct image_part {
  std::string uri{};
};

struct audio_part {
  std::string base64{};
  std::string uri{};
};

struct video_part {
  std::string uri{};
};

struct file_part {
  std::string uri{};
  std::string mime_type{};
};

struct tool_call_part {
  std::size_t index{0U};
  std::string id{};
  std::string type{"function"};
  std::string name{};
  std::string arguments{};
  bool complete{true};
};

using message_part = std::variant<text_part, image_part, audio_part, video_part,
                                  file_part, tool_call_part>;

struct token_usage {
  std::int64_t prompt_tokens{0};
  std::int64_t completion_tokens{0};
  std::int64_t total_tokens{0};
};

struct logprob_entry {
  std::string token{};
  double logprob{0.0};
};

struct response_meta {
  std::string finish_reason{};
  token_usage usage{};
  std::vector<logprob_entry> logprobs{};
};

struct message {
  std::string message_id{};
  message_role role{message_role::user};
  std::string name{};
  std::string tool_call_id{};
  std::string tool_name{};
  std::vector<message_part> parts{};
  response_meta meta{};
};

enum class message_update_mode {
  append,
  upsert_by_id,
};

enum class template_format {
  light,
  logic,
  script,
};

enum class placeholder_type {
  text,
  number,
  boolean,
};

struct placeholder_value {
  std::string value{};
  bool optional{false};
  placeholder_type type{placeholder_type::text};
};

enum class message_update_action {
  appended,
  inserted,
  replaced,
  rejected,
};

struct message_update_audit_entry {
  message_update_mode mode{message_update_mode::append};
  message_update_action action{message_update_action::appended};
  std::string message_id{};
  wh::core::error_code error{};
};

struct message_string_hash {
  using is_transparent = void;

  [[nodiscard]] auto operator()(const std::string_view value) const noexcept
      -> std::size_t {
    return std::hash<std::string_view>{}(value);
  }

  [[nodiscard]] auto operator()(const std::string &value) const noexcept
      -> std::size_t {
    return (*this)(std::string_view{value});
  }

  [[nodiscard]] auto operator()(const char *value) const noexcept
      -> std::size_t {
    return (*this)(std::string_view{value});
  }
};

struct message_string_equal {
  using is_transparent = void;

  [[nodiscard]] auto operator()(const std::string_view left,
                                const std::string_view right) const noexcept
      -> bool {
    return left == right;
  }
};

using placeholder_context =
    std::unordered_map<std::string, placeholder_value, message_string_hash,
                       message_string_equal>;

namespace detail {

struct template_delimiters {
  std::string_view begin{};
  std::string_view end{};
};

[[nodiscard]] inline auto is_identity_compatible(const message &left,
                                                 const message &right) noexcept
    -> bool {
  if (left.role != right.role) {
    return false;
  }
  if (!left.name.empty() && !right.name.empty() && left.name != right.name) {
    return false;
  }
  if (!left.tool_call_id.empty() && !right.tool_call_id.empty() &&
      left.tool_call_id != right.tool_call_id) {
    return false;
  }
  if (!left.tool_name.empty() && !right.tool_name.empty() &&
      left.tool_name != right.tool_name) {
    return false;
  }
  return true;
}

inline auto merge_usage(token_usage &target, const token_usage &next) noexcept
    -> void {
  target.prompt_tokens = std::max(target.prompt_tokens, next.prompt_tokens);
  target.completion_tokens =
      std::max(target.completion_tokens, next.completion_tokens);
  target.total_tokens = std::max(target.total_tokens, next.total_tokens);
}

[[nodiscard]] inline auto can_merge_adjacent_parts(const message_part &left,
                                                   const message_part &right)
    -> bool {
  if (const auto *left_text = std::get_if<text_part>(&left);
      left_text != nullptr) {
    return std::holds_alternative<text_part>(right);
  }

  const auto *left_audio = std::get_if<audio_part>(&left);
  const auto *right_audio = std::get_if<audio_part>(&right);
  if (left_audio == nullptr || right_audio == nullptr) {
    return false;
  }

  return left_audio->uri.empty() && right_audio->uri.empty() &&
         !left_audio->base64.empty() && !right_audio->base64.empty();
}

inline auto merge_adjacent_into(message_part &target,
                                const message_part &source) -> void {
  if (auto *target_text = std::get_if<text_part>(&target);
      target_text != nullptr) {
    target_text->text += std::get<text_part>(source).text;
    return;
  }

  auto *target_audio = std::get_if<audio_part>(&target);
  if (target_audio != nullptr) {
    target_audio->base64 += std::get<audio_part>(source).base64;
  }
}

[[nodiscard]] inline auto
normalize_message_parts(const std::span<const message_part> parts)
    -> wh::core::result<std::vector<message_part>> {
  std::vector<message_part> non_tool_parts{};
  std::vector<tool_call_part> tool_calls{};
  std::unordered_map<std::size_t, std::size_t> tool_call_position{};
  non_tool_parts.reserve(parts.size());
  tool_calls.reserve(parts.size());
  tool_call_position.reserve(parts.size());

  for (const auto &part : parts) {
    if (const auto *tool_call = std::get_if<tool_call_part>(&part);
        tool_call != nullptr) {
      const auto position_iter = tool_call_position.find(tool_call->index);
      if (position_iter == tool_call_position.end()) {
        tool_call_position.emplace(tool_call->index, tool_calls.size());
        tool_calls.push_back(*tool_call);
        continue;
      }

      auto &existing = tool_calls[position_iter->second];
      if ((!existing.id.empty() && !tool_call->id.empty() &&
           existing.id != tool_call->id) ||
          (!existing.type.empty() && !tool_call->type.empty() &&
           existing.type != tool_call->type) ||
          (!existing.name.empty() && !tool_call->name.empty() &&
           existing.name != tool_call->name)) {
        return wh::core::result<std::vector<message_part>>::failure(
            wh::core::errc::contract_violation);
      }

      if (existing.id.empty()) {
        existing.id = tool_call->id;
      }
      if (existing.type.empty()) {
        existing.type = tool_call->type;
      }
      if (existing.name.empty()) {
        existing.name = tool_call->name;
      }
      existing.arguments += tool_call->arguments;
      existing.complete = existing.complete && tool_call->complete;
      continue;
    }

    if (!non_tool_parts.empty() &&
        can_merge_adjacent_parts(non_tool_parts.back(), part)) {
      merge_adjacent_into(non_tool_parts.back(), part);
      continue;
    }
    non_tool_parts.push_back(part);
  }

  std::ranges::sort(tool_calls,
                    [](const tool_call_part &left,
                       const tool_call_part &right) noexcept -> bool {
                      return left.index < right.index;
                    });
  for (auto &tool_call : tool_calls) {
    non_tool_parts.emplace_back(std::move(tool_call));
  }

  if (non_tool_parts.empty()) {
    return wh::core::result<std::vector<message_part>>::failure(
        wh::core::errc::protocol_error);
  }
  return non_tool_parts;
}

[[nodiscard]] inline auto
has_forbidden_script_token(const std::string_view input) -> bool {
  static constexpr std::array<std::string_view, 4U> forbidden_tokens{
      "include", "extends", "import", "from"};
  return std::ranges::any_of(
      forbidden_tokens, [&](const std::string_view token) noexcept -> bool {
        return input.find(token) != std::string_view::npos;
      });
}

[[nodiscard]] inline auto trim(std::string_view input) -> std::string_view {
  while (!input.empty() &&
         std::isspace(static_cast<unsigned char>(input.front()))) {
    input.remove_prefix(1U);
  }
  while (!input.empty() &&
         std::isspace(static_cast<unsigned char>(input.back()))) {
    input.remove_suffix(1U);
  }
  return input;
}

[[nodiscard]] inline auto is_numeric_placeholder(
    const std::string_view input) -> bool {
  const auto value = trim(input);
  if (value.empty()) {
    return false;
  }
  const std::string normalized{value};
  errno = 0;
  char *end = nullptr;
  const auto parsed = std::strtod(normalized.c_str(), &end);
  static_cast<void>(parsed);
  return errno == 0 && end != nullptr && *end == '\0';
}

[[nodiscard]] inline auto is_valid_placeholder_value(
    const placeholder_value &placeholder) -> bool {
  switch (placeholder.type) {
  case placeholder_type::text:
    return true;
  case placeholder_type::number:
    return is_numeric_placeholder(placeholder.value);
  case placeholder_type::boolean:
    return placeholder.value == "true" || placeholder.value == "false";
  }
  return false;
}

[[nodiscard]] inline auto resolve_template_delimiters(const template_format format)
    -> template_delimiters {
  switch (format) {
  case template_format::light:
    return {"{{", "}}"};
  case template_format::logic:
    return {"${", "}"};
  case template_format::script:
    return {"{%", "%}"};
  }
  return {"{{", "}}"};
}

} // namespace detail

[[nodiscard]] inline auto
merge_message_chunks(const std::span<const message> chunks)
    -> wh::core::result<message> {
  if (chunks.empty()) {
    return wh::core::result<message>::failure(wh::core::errc::invalid_argument);
  }
  if (chunks.front().parts.empty()) {
    return wh::core::result<message>::failure(wh::core::errc::protocol_error);
  }

  std::size_t total_parts = 0U;
  std::size_t total_logprobs = 0U;
  for (const auto &chunk : chunks) {
    total_parts += chunk.parts.size();
    total_logprobs += chunk.meta.logprobs.size();
  }

  message merged = chunks.front();
  merged.meta.logprobs.reserve(total_logprobs);
  std::vector<message_part> collected_parts{};
  collected_parts.reserve(total_parts);
  collected_parts.insert(collected_parts.end(), chunks.front().parts.begin(),
                         chunks.front().parts.end());
  for (const auto &chunk : chunks | std::views::drop(1U)) {
    if (chunk.parts.empty()) {
      return wh::core::result<message>::failure(wh::core::errc::protocol_error);
    }
    if (!detail::is_identity_compatible(merged, chunk)) {
      return wh::core::result<message>::failure(
          wh::core::errc::contract_violation);
    }

    if (merged.name.empty()) {
      merged.name = chunk.name;
    }
    if (merged.tool_call_id.empty()) {
      merged.tool_call_id = chunk.tool_call_id;
    }
    if (merged.tool_name.empty()) {
      merged.tool_name = chunk.tool_name;
    }

    if (!chunk.meta.finish_reason.empty()) {
      merged.meta.finish_reason = chunk.meta.finish_reason;
    }
    detail::merge_usage(merged.meta.usage, chunk.meta.usage);
    merged.meta.logprobs.insert(merged.meta.logprobs.end(),
                                chunk.meta.logprobs.begin(),
                                chunk.meta.logprobs.end());

    collected_parts.insert(collected_parts.end(), chunk.parts.begin(),
                           chunk.parts.end());
  }

  auto normalized = detail::normalize_message_parts(collected_parts);
  if (normalized.has_error()) {
    return wh::core::result<message>::failure(normalized.error());
  }
  merged.parts = std::move(normalized).value();
  return merged;
}

inline auto apply_message_update(
    std::vector<message> &messages, const message &delta,
    const message_update_mode mode = message_update_mode::append,
    std::vector<message_update_audit_entry> *const audit_log = nullptr)
    -> wh::core::result<void> {
  const auto push_audit = [&](const message_update_action action,
                              const wh::core::error_code error = {})
      -> void {
    if (audit_log == nullptr) {
      return;
    }
    audit_log->push_back(message_update_audit_entry{
        mode, action, delta.message_id, error,
    });
  };

  if (mode == message_update_mode::append) {
    messages.push_back(delta);
    push_audit(message_update_action::appended);
    return {};
  }

  if (delta.message_id.empty()) {
    push_audit(message_update_action::rejected,
               wh::core::make_error(wh::core::errc::invalid_argument));
    return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
  }

  const auto iter =
      std::ranges::find_if(messages, [&](const message &existing) {
        return existing.message_id == delta.message_id;
      });
  if (iter == messages.end()) {
    messages.push_back(delta);
    push_audit(message_update_action::inserted);
    return {};
  }

  if (iter->role != delta.role) {
    push_audit(message_update_action::rejected,
               wh::core::make_error(wh::core::errc::contract_violation));
    return wh::core::result<void>::failure(wh::core::errc::contract_violation);
  }
  *iter = delta;
  push_audit(message_update_action::replaced);
  return {};
}

[[nodiscard]] inline auto
render_message_template(const std::string_view text,
                        const placeholder_context &context,
                        const template_format format = template_format::light)
    -> wh::core::result<std::string> {
  if (format == template_format::script &&
      detail::has_forbidden_script_token(text)) {
    return wh::core::result<std::string>::failure(
        wh::core::errc::contract_violation);
  }

  const auto delimiters = detail::resolve_template_delimiters(format);
  std::string rendered;
  rendered.reserve(text.size());
  std::size_t cursor = 0U;
  while (cursor < text.size()) {
    const auto begin = text.find(delimiters.begin, cursor);
    if (begin == std::string_view::npos) {
      rendered.append(text.substr(cursor));
      break;
    }

    rendered.append(text.substr(cursor, begin - cursor));
    const auto end =
        text.find(delimiters.end, begin + delimiters.begin.size());
    if (end == std::string_view::npos) {
      return wh::core::result<std::string>::failure(
          wh::core::errc::parse_error);
    }

    const auto key =
        detail::trim(text.substr(begin + delimiters.begin.size(),
                                 end - (begin + delimiters.begin.size())));
    const auto iter = context.find(key);
    if (iter == context.end()) {
      return wh::core::result<std::string>::failure(wh::core::errc::not_found);
    }
    if (!detail::is_valid_placeholder_value(iter->second)) {
      return wh::core::result<std::string>::failure(
          wh::core::errc::type_mismatch);
    }
    if (!iter->second.optional || !iter->second.value.empty()) {
      rendered += iter->second.value;
    }
    cursor = end + delimiters.end.size();
  }

  return rendered;
}

} // namespace wh::schema
