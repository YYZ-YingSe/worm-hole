// Defines message schema structures (roles, parts, metadata, tool calls)
// used across prompt, model, and chain components.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <ranges>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/type_traits.hpp"

namespace wh::schema {

/// Logical role of a message in chat-style interaction.
enum class message_role { system, user, assistant, tool };

/// Plain text content part.
struct text_part {
  /// UTF-8 text payload.
  std::string text{};
};

/// Image reference content part.
struct image_part {
  /// URI or provider-specific image locator.
  std::string uri{};
};

/// Audio content part (inline base64 or URI).
struct audio_part {
  /// Inline base64 audio payload when streaming inline blobs.
  std::string base64{};
  /// URI for externally hosted audio.
  std::string uri{};
};

/// Video reference content part.
struct video_part {
  /// URI for externally hosted video.
  std::string uri{};
};

/// Generic file content part.
struct file_part {
  /// URI for externally hosted file.
  std::string uri{};
  /// MIME type used by adapter/model-side routing.
  std::string mime_type{};
};

/// Tool call delta/final payload part.
struct tool_call_part {
  /// Tool call position used to reassemble deltas deterministically.
  std::size_t index{0U};
  /// Stable call id returned by provider.
  std::string id{};
  /// Tool call kind, normally `function`.
  std::string type{"function"};
  /// Tool/function name.
  std::string name{};
  /// Tool arguments payload (often JSON string).
  std::string arguments{};
  /// Whether this call is fully emitted.
  bool complete{true};
};

/// Union of all supported message content parts.
using message_part =
    std::variant<text_part, image_part, audio_part, video_part, file_part, tool_call_part>;

/// Token usage counters attached to model responses.
struct token_usage {
  /// Prompt token count.
  std::int64_t prompt_tokens{0};
  /// Completion token count.
  std::int64_t completion_tokens{0};
  /// End-to-end token count.
  std::int64_t total_tokens{0};
};

/// Per-token logprob entry.
struct logprob_entry {
  /// Token text emitted by provider.
  std::string token{};
  /// Log probability for the token.
  double logprob{0.0};
};

/// Additional response metadata carried with a message.
struct response_meta {
  /// Stop reason from model provider.
  std::string finish_reason{};
  /// Aggregated usage accounting.
  token_usage usage{};
  /// Optional per-token logprobs.
  std::vector<logprob_entry> logprobs{};
};

/// Canonical multi-part message representation.
struct message {
  /// Provider-level message id when available.
  std::string message_id{};
  /// Sender role in conversation.
  message_role role{message_role::user};
  /// Optional participant name.
  std::string name{};
  /// Tool-call correlation id for tool role messages.
  std::string tool_call_id{};
  /// Tool name for tool role messages.
  std::string tool_name{};
  /// Ordered multimodal/tool parts.
  std::vector<message_part> parts{};
  /// Response-level metadata.
  response_meta meta{};
};

/// Update strategy when applying message deltas.
enum class message_update_mode {
  /// Append incoming message fragments in order.
  append,
  /// Insert or replace message by stable identifier.
  upsert_by_id,
};

/// Audit action emitted by message update pipeline.
enum class message_update_action {
  /// Update operation appended message content.
  appended,
  /// Update operation inserted a new message.
  inserted,
  /// Update operation replaced existing message content.
  replaced,
  /// Update operation was rejected by policy/validation.
  rejected,
};

/// Audit record for a single message update operation.
struct message_update_audit_entry {
  /// Update mode used for this operation.
  message_update_mode mode{message_update_mode::append};
  /// Result action observed for this operation.
  message_update_action action{message_update_action::appended};
  /// Message id associated with the delta.
  std::string message_id{};
  /// Error when action is rejected.
  wh::core::error_code error{};
};

/// Transparent hash alias for message-related string maps.
using message_string_hash = wh::core::transparent_string_hash;

/// Transparent equality alias for message string maps.
using message_string_equal = wh::core::transparent_string_equal;

/// Optional accelerator index from `message_id` to vector position.
using message_index =
    std::unordered_map<std::string, std::size_t, message_string_hash, message_string_equal>;

namespace detail {

/// Verifies that two message chunks can be merged safely.
[[nodiscard]] inline auto is_identity_compatible(const message &left, const message &right) noexcept
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
  if (!left.tool_name.empty() && !right.tool_name.empty() && left.tool_name != right.tool_name) {
    return false;
  }
  return true;
}

/// Merges token usage by taking max counters from chunks.
inline auto merge_usage(token_usage &target, const token_usage &next) noexcept -> void {
  target.prompt_tokens = std::max(target.prompt_tokens, next.prompt_tokens);
  target.completion_tokens = std::max(target.completion_tokens, next.completion_tokens);
  target.total_tokens = std::max(target.total_tokens, next.total_tokens);
}

/// Returns whether two adjacent parts are mergeable.
[[nodiscard]] inline auto can_merge_adjacent_parts(const message_part &left,
                                                   const message_part &right) -> bool {
  if (const auto *left_text = std::get_if<text_part>(&left); left_text != nullptr) {
    return std::holds_alternative<text_part>(right);
  }

  const auto *left_audio = std::get_if<audio_part>(&left);
  const auto *right_audio = std::get_if<audio_part>(&right);
  if (left_audio == nullptr || right_audio == nullptr) {
    return false;
  }

  return left_audio->uri.empty() && right_audio->uri.empty() && !left_audio->base64.empty() &&
         !right_audio->base64.empty();
}

/// In-place merge for adjacent mergeable parts.
inline auto merge_adjacent_into(message_part &target, const message_part &source) -> void {
  if (auto *target_text = std::get_if<text_part>(&target); target_text != nullptr) {
    target_text->text.append(std::get<text_part>(source).text);
    return;
  }

  auto *target_audio = std::get_if<audio_part>(&target);
  if (target_audio != nullptr) {
    target_audio->base64.append(std::get<audio_part>(source).base64);
  }
}

/// Normalizes mixed parts: merges adjacents and combines tool-call deltas.
[[nodiscard]] inline auto normalize_message_parts(const std::span<const message_part> parts)
    -> wh::core::result<std::vector<message_part>> {
  std::vector<message_part> non_tool_parts{};
  std::vector<tool_call_part> tool_calls{};
  std::unordered_map<std::size_t, std::size_t> tool_call_position{};
  non_tool_parts.reserve(parts.size());
  tool_calls.reserve(parts.size());
  tool_call_position.reserve(parts.size());

  for (const auto &part : parts) {
    if (const auto *tool_call = std::get_if<tool_call_part>(&part); tool_call != nullptr) {
      const auto position_iter = tool_call_position.find(tool_call->index);
      if (position_iter == tool_call_position.end()) {
        tool_call_position.emplace(tool_call->index, tool_calls.size());
        tool_calls.push_back(*tool_call);
        continue;
      }

      auto &existing = tool_calls[position_iter->second];
      if ((!existing.id.empty() && !tool_call->id.empty() && existing.id != tool_call->id) ||
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
      existing.arguments.append(tool_call->arguments);
      existing.complete = existing.complete && tool_call->complete;
      continue;
    }

    if (!non_tool_parts.empty() && can_merge_adjacent_parts(non_tool_parts.back(), part)) {
      merge_adjacent_into(non_tool_parts.back(), part);
      continue;
    }
    non_tool_parts.push_back(part);
  }

  std::ranges::sort(tool_calls,
                    [](const tool_call_part &left, const tool_call_part &right) noexcept -> bool {
                      return left.index < right.index;
                    });
  for (auto &tool_call : tool_calls) {
    non_tool_parts.emplace_back(std::move(tool_call));
  }

  if (non_tool_parts.empty()) {
    return wh::core::result<std::vector<message_part>>::failure(wh::core::errc::protocol_error);
  }
  return non_tool_parts;
}

/// Move-overload of `normalize_message_parts` for lower copy overhead.
[[nodiscard]] inline auto normalize_message_parts(std::vector<message_part> &&parts)
    -> wh::core::result<std::vector<message_part>> {
  std::vector<message_part> non_tool_parts{};
  std::vector<tool_call_part> tool_calls{};
  std::unordered_map<std::size_t, std::size_t> tool_call_position{};
  non_tool_parts.reserve(parts.size());
  tool_calls.reserve(parts.size());
  tool_call_position.reserve(parts.size());

  for (auto &part : parts) {
    if (auto *tool_call = std::get_if<tool_call_part>(&part); tool_call != nullptr) {
      const auto position_iter = tool_call_position.find(tool_call->index);
      if (position_iter == tool_call_position.end()) {
        tool_call_position.emplace(tool_call->index, tool_calls.size());
        tool_calls.push_back(std::move(*tool_call));
        continue;
      }

      auto &existing = tool_calls[position_iter->second];
      if ((!existing.id.empty() && !tool_call->id.empty() && existing.id != tool_call->id) ||
          (!existing.type.empty() && !tool_call->type.empty() &&
           existing.type != tool_call->type) ||
          (!existing.name.empty() && !tool_call->name.empty() &&
           existing.name != tool_call->name)) {
        return wh::core::result<std::vector<message_part>>::failure(
            wh::core::errc::contract_violation);
      }

      if (existing.id.empty()) {
        existing.id = std::move(tool_call->id);
      }
      if (existing.type.empty()) {
        existing.type = std::move(tool_call->type);
      }
      if (existing.name.empty()) {
        existing.name = std::move(tool_call->name);
      }
      existing.arguments.append(tool_call->arguments);
      existing.complete = existing.complete && tool_call->complete;
      continue;
    }

    if (!non_tool_parts.empty() && can_merge_adjacent_parts(non_tool_parts.back(), part)) {
      merge_adjacent_into(non_tool_parts.back(), part);
      continue;
    }
    non_tool_parts.push_back(std::move(part));
  }

  std::ranges::sort(tool_calls,
                    [](const tool_call_part &left, const tool_call_part &right) noexcept -> bool {
                      return left.index < right.index;
                    });
  for (auto &tool_call : tool_calls) {
    non_tool_parts.emplace_back(std::move(tool_call));
  }

  if (non_tool_parts.empty()) {
    return wh::core::result<std::vector<message_part>>::failure(wh::core::errc::protocol_error);
  }
  return non_tool_parts;
}

} // namespace detail

/// Merges message chunks into one normalized message (copy path).
[[nodiscard]] inline auto merge_message_chunks(const std::span<const message> chunks)
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
      return wh::core::result<message>::failure(wh::core::errc::contract_violation);
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
    merged.meta.logprobs.insert(merged.meta.logprobs.end(), chunk.meta.logprobs.begin(),
                                chunk.meta.logprobs.end());

    collected_parts.insert(collected_parts.end(), chunk.parts.begin(), chunk.parts.end());
  }

  auto normalized = detail::normalize_message_parts(std::move(collected_parts));
  if (normalized.has_error()) {
    return wh::core::result<message>::failure(normalized.error());
  }
  merged.parts = std::move(normalized).value();
  return merged;
}

/// Merges message chunks into one normalized message (move path).
[[nodiscard]] inline auto merge_message_chunks(std::vector<message> &&chunks)
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

  message merged = std::move(chunks.front());
  merged.meta.logprobs.reserve(total_logprobs);
  std::vector<message_part> collected_parts{};
  collected_parts.reserve(total_parts);
  collected_parts.insert(collected_parts.end(), std::make_move_iterator(merged.parts.begin()),
                         std::make_move_iterator(merged.parts.end()));
  merged.parts.clear();

  for (auto &chunk : chunks | std::views::drop(1U)) {
    if (chunk.parts.empty()) {
      return wh::core::result<message>::failure(wh::core::errc::protocol_error);
    }
    if (!detail::is_identity_compatible(merged, chunk)) {
      return wh::core::result<message>::failure(wh::core::errc::contract_violation);
    }

    if (merged.name.empty()) {
      merged.name = std::move(chunk.name);
    }
    if (merged.tool_call_id.empty()) {
      merged.tool_call_id = std::move(chunk.tool_call_id);
    }
    if (merged.tool_name.empty()) {
      merged.tool_name = std::move(chunk.tool_name);
    }

    if (!chunk.meta.finish_reason.empty()) {
      merged.meta.finish_reason = std::move(chunk.meta.finish_reason);
    }
    detail::merge_usage(merged.meta.usage, chunk.meta.usage);
    merged.meta.logprobs.insert(merged.meta.logprobs.end(),
                                std::make_move_iterator(chunk.meta.logprobs.begin()),
                                std::make_move_iterator(chunk.meta.logprobs.end()));

    collected_parts.insert(collected_parts.end(), std::make_move_iterator(chunk.parts.begin()),
                           std::make_move_iterator(chunk.parts.end()));
  }

  auto normalized = detail::normalize_message_parts(std::move(collected_parts));
  if (normalized.has_error()) {
    return wh::core::result<message>::failure(normalized.error());
  }
  merged.parts = std::move(normalized).value();
  return merged;
}

/// Applies one message delta with optional audit log (linear scan path).
template <typename MessageType>
  requires std::same_as<wh::core::remove_cvref_t<MessageType>, message>
inline auto apply_message_update(std::vector<message> &messages, MessageType &&delta,
                                 const message_update_mode mode = message_update_mode::append,
                                 std::vector<message_update_audit_entry> *const audit_log = nullptr)
    -> wh::core::result<void> {
  const std::string delta_id = delta.message_id;
  const message_role delta_role = delta.role;

  const auto push_audit = [&](const message_update_action action,
                              const wh::core::error_code error = {}) -> void {
    if (audit_log == nullptr) {
      return;
    }
    audit_log->push_back(message_update_audit_entry{
        mode,
        action,
        delta_id,
        error,
    });
  };

  if (mode == message_update_mode::append) {
    messages.push_back(std::forward<MessageType>(delta));
    push_audit(message_update_action::appended);
    return {};
  }

  if (delta_id.empty()) {
    push_audit(message_update_action::rejected,
               wh::core::make_error(wh::core::errc::invalid_argument));
    return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
  }

  const auto iter = std::ranges::find_if(
      messages, [&](const message &existing) { return existing.message_id == delta_id; });
  if (iter == messages.end()) {
    messages.push_back(std::forward<MessageType>(delta));
    push_audit(message_update_action::inserted);
    return {};
  }

  if (iter->role != delta_role) {
    push_audit(message_update_action::rejected,
               wh::core::make_error(wh::core::errc::contract_violation));
    return wh::core::result<void>::failure(wh::core::errc::contract_violation);
  }
  *iter = std::forward<MessageType>(delta);
  push_audit(message_update_action::replaced);
  return {};
}

/// Applies one message delta with external id-index acceleration.
template <typename MessageType>
  requires std::same_as<wh::core::remove_cvref_t<MessageType>, message>
inline auto apply_message_update(std::vector<message> &messages, MessageType &&delta,
                                 message_index &index,
                                 const message_update_mode mode = message_update_mode::append,
                                 std::vector<message_update_audit_entry> *const audit_log = nullptr)
    -> wh::core::result<void> {
  const std::string delta_id = delta.message_id;
  const message_role delta_role = delta.role;

  const auto push_audit = [&](const message_update_action action,
                              const wh::core::error_code error = {}) -> void {
    if (audit_log == nullptr) {
      return;
    }
    audit_log->push_back(message_update_audit_entry{
        mode,
        action,
        delta_id,
        error,
    });
  };

  const auto update_index = [&](const std::string &id, std::size_t pos) {
    if (!id.empty()) {
      index[id] = pos;
    }
  };

  if (index.empty() && !messages.empty()) {
    index.reserve(messages.size());
    for (std::size_t i = 0; i < messages.size(); ++i) {
      if (!messages[i].message_id.empty()) {
        index[messages[i].message_id] = i;
      }
    }
  }

  if (mode == message_update_mode::append) {
    const auto pos = messages.size();
    messages.push_back(std::forward<MessageType>(delta));
    update_index(delta_id, pos);
    push_audit(message_update_action::appended);
    return {};
  }

  if (delta_id.empty()) {
    push_audit(message_update_action::rejected,
               wh::core::make_error(wh::core::errc::invalid_argument));
    return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
  }

  const auto index_iter = index.find(delta_id);
  if (index_iter == index.end()) {
    const auto pos = messages.size();
    messages.push_back(std::forward<MessageType>(delta));
    update_index(delta_id, pos);
    push_audit(message_update_action::inserted);
    return {};
  }

  auto iter = messages.begin() + static_cast<std::ptrdiff_t>(index_iter->second);
  if (iter->role != delta_role) {
    push_audit(message_update_action::rejected,
               wh::core::make_error(wh::core::errc::contract_violation));
    return wh::core::result<void>::failure(wh::core::errc::contract_violation);
  }
  *iter = std::forward<MessageType>(delta);
  push_audit(message_update_action::replaced);
  return {};
}

} // namespace wh::schema
