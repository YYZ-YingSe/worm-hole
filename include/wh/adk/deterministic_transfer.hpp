// Defines deterministic transfer bridge helpers for authored agent routing,
// history rewriting, and idempotent transfer-message emission.
#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "wh/adk/call_options.hpp"
#include "wh/adk/types.hpp"
#include "wh/agent/agent.hpp"
#include "wh/core/result.hpp"
#include "wh/core/type_traits.hpp"
#include "wh/schema/message/types.hpp"

namespace wh::adk {

/// Stable tool name used for transfer assistant/tool message pairs.
inline constexpr std::string_view deterministic_transfer_tool_name = "transfer_to_agent";

/// Supported transfer directions resolved by the deterministic bridge.
enum class transfer_target_kind {
  /// Stay on the current agent.
  current = 0U,
  /// Transfer upward to the parent agent.
  parent,
  /// Transfer downward to one named child agent.
  child,
};

/// One authored transfer target description.
struct transfer_target {
  /// Direction resolved by this transfer request.
  transfer_target_kind kind{transfer_target_kind::current};
  /// Child-agent name used when `kind == child`.
  std::string agent_name{};
};

/// Terminal reason used when deciding whether transfer messages may be
/// appended.
enum class transfer_completion_kind {
  /// The transfer finished normally and may append history.
  normal = 0U,
  /// The current path exited explicitly and must not append transfer history.
  exit,
  /// The current path interrupted and must not append transfer history.
  interrupt,
};

/// Bridge-local deterministic transfer state that can be checkpointed or
/// projected outward without leaking runtime internals.
struct deterministic_transfer_state {
  /// Exact run-path that is allowed to contribute parent-visible history.
  run_path exact_run_path{};
  /// Parent-visible history captured through exact-path gating.
  std::vector<wh::schema::message> visible_history{};
  /// Agent names already visited in the current transfer chain.
  std::unordered_set<std::string, wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      visited_agents{};
  /// Transfer tool-call ids already appended to visible history.
  std::unordered_set<std::string, wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      appended_tool_call_ids{};
  /// Pending resolved transfer target stored until normal completion.
  std::optional<std::string> pending_target{};
};

namespace detail::transfer {

[[nodiscard]] inline auto make_context_message(std::string text) -> wh::schema::message {
  wh::schema::message message{};
  message.role = wh::schema::message_role::user;
  message.parts.emplace_back(wh::schema::text_part{std::move(text)});
  return message;
}

[[nodiscard]] inline auto render_message_text(const wh::schema::message &message) -> std::string {
  std::string text{};
  for (const auto &part : message.parts) {
    if (const auto *typed = std::get_if<wh::schema::text_part>(&part); typed != nullptr) {
      if (!text.empty()) {
        text.push_back(' ');
      }
      text.append(typed->text);
      continue;
    }
    if (const auto *tool = std::get_if<wh::schema::tool_call_part>(&part); tool != nullptr) {
      if (!text.empty()) {
        text.push_back(' ');
      }
      text.append("[tool:");
      text.append(tool->name);
      text.push_back(']');
    }
  }
  return text;
}

[[nodiscard]] inline auto make_context_text(const wh::schema::message &message) -> std::string {
  std::string text{"[context"};
  if (!message.name.empty()) {
    text.append(" agent=");
    text.append(message.name);
  }
  if (!message.tool_name.empty()) {
    text.append(" tool=");
    text.append(message.tool_name);
  }
  text.append("] ");
  text.append(render_message_text(message));
  return text;
}

[[nodiscard]] inline auto is_transfer_assistant_message(const wh::schema::message &message)
    -> bool {
  if (message.role != wh::schema::message_role::assistant) {
    return false;
  }
  for (const auto &part : message.parts) {
    const auto *tool = std::get_if<wh::schema::tool_call_part>(&part);
    if (tool != nullptr && tool->name == deterministic_transfer_tool_name) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] inline auto transfer_assistant_call_id(const wh::schema::message &message)
    -> std::optional<std::string_view> {
  if (!is_transfer_assistant_message(message)) {
    return std::nullopt;
  }
  for (const auto &part : message.parts) {
    const auto *tool = std::get_if<wh::schema::tool_call_part>(&part);
    if (tool != nullptr && tool->name == deterministic_transfer_tool_name) {
      return std::string_view{tool->id};
    }
  }
  return std::nullopt;
}

[[nodiscard]] inline auto is_transfer_tool_message(const wh::schema::message &message) -> bool {
  return message.role == wh::schema::message_role::tool &&
         message.tool_name == deterministic_transfer_tool_name && !message.tool_call_id.empty();
}

} // namespace detail::transfer

/// Builds the assistant-side transfer tool-call message.
[[nodiscard]] inline auto make_transfer_assistant_message(const std::string_view target_agent_name,
                                                          const std::string_view tool_call_id)
    -> wh::schema::message {
  wh::schema::message message{};
  message.role = wh::schema::message_role::assistant;
  message.parts.emplace_back(wh::schema::tool_call_part{
      .index = 0U,
      .id = std::string{tool_call_id},
      .type = "function",
      .name = std::string{deterministic_transfer_tool_name},
      .arguments = std::string{target_agent_name},
      .complete = true,
  });
  return message;
}

/// Builds the tool-side transfer acknowledgement message.
[[nodiscard]] inline auto make_transfer_tool_message(const std::string_view target_agent_name,
                                                     const std::string_view tool_call_id)
    -> wh::schema::message {
  wh::schema::message message{};
  message.role = wh::schema::message_role::tool;
  message.tool_call_id = std::string{tool_call_id};
  message.tool_name = std::string{deterministic_transfer_tool_name};
  message.parts.emplace_back(
      wh::schema::text_part{std::string{"transfer:"} + std::string{target_agent_name}});
  return message;
}

/// Parses one normalized transfer action from a transfer tool message.
[[nodiscard]] inline auto parse_transfer_tool_message(const wh::schema::message &message)
    -> wh::core::result<wh::agent::agent_transfer> {
  if (!detail::transfer::is_transfer_tool_message(message)) {
    return wh::core::result<wh::agent::agent_transfer>::failure(wh::core::errc::type_mismatch);
  }
  if (message.parts.empty()) {
    return wh::core::result<wh::agent::agent_transfer>::failure(wh::core::errc::invalid_argument);
  }

  const auto *text = std::get_if<wh::schema::text_part>(&message.parts.front());
  if (text == nullptr) {
    return wh::core::result<wh::agent::agent_transfer>::failure(wh::core::errc::type_mismatch);
  }

  constexpr std::string_view prefix = "transfer:";
  if (!text->text.starts_with(prefix)) {
    return wh::core::result<wh::agent::agent_transfer>::failure(wh::core::errc::invalid_argument);
  }

  auto target_agent_name = text->text.substr(prefix.size());
  if (target_agent_name.empty() || message.tool_call_id.empty()) {
    return wh::core::result<wh::agent::agent_transfer>::failure(wh::core::errc::invalid_argument);
  }

  return wh::agent::agent_transfer{
      .target_agent_name = std::move(target_agent_name),
      .tool_call_id = message.tool_call_id,
  };
}

/// Best-effort transfer extractor for one agent final message.
[[nodiscard]] inline auto extract_transfer_from_message(const wh::schema::message &message)
    -> std::optional<wh::agent::agent_transfer> {
  auto parsed = parse_transfer_tool_message(message);
  if (parsed.has_error()) {
    return std::nullopt;
  }
  return std::move(parsed).value();
}

/// Resolves one raw target name against the authored parent/child topology of
/// the current agent.
[[nodiscard]] inline auto resolve_transfer_target(const wh::agent::agent &current,
                                                  const std::string_view target_agent_name)
    -> wh::core::result<transfer_target> {
  if (target_agent_name.empty()) {
    return wh::core::result<transfer_target>::failure(wh::core::errc::invalid_argument);
  }

  if (target_agent_name == current.name()) {
    return transfer_target{
        .kind = transfer_target_kind::current,
    };
  }

  if (current.parent_name().has_value() && target_agent_name == *current.parent_name()) {
    if (!current.allows_transfer_to_parent()) {
      return wh::core::result<transfer_target>::failure(wh::core::errc::contract_violation);
    }
    return transfer_target{
        .kind = transfer_target_kind::parent,
    };
  }

  if (current.has_child(target_agent_name)) {
    if (!current.allows_transfer_to_child(target_agent_name)) {
      return wh::core::result<transfer_target>::failure(wh::core::errc::contract_violation);
    }
    return transfer_target{
        .kind = transfer_target_kind::child,
        .agent_name = std::string{target_agent_name},
    };
  }

  return wh::core::result<transfer_target>::failure(wh::core::errc::not_found);
}

/// Starts one deterministic transfer after the target name has already been
/// validated and resolved.
[[nodiscard]] inline auto begin_deterministic_transfer(deterministic_transfer_state &state,
                                                       const std::string_view resolved_target)
    -> wh::core::result<std::string> {
  if (resolved_target.empty()) {
    return wh::core::result<std::string>::failure(wh::core::errc::invalid_argument);
  }
  if (state.visited_agents.contains(resolved_target)) {
    return wh::core::result<std::string>::failure(wh::core::errc::already_exists);
  }
  state.visited_agents.insert(std::string{resolved_target});
  state.pending_target = std::string{resolved_target};
  return std::string{resolved_target};
}

/// Resolves one transfer target against authored agent topology and records
/// the pending target plus loop-guard visit state.
[[nodiscard]] inline auto begin_deterministic_transfer(const wh::agent::agent &current,
                                                       deterministic_transfer_state &state,
                                                       transfer_target target)
    -> wh::core::result<std::string> {
  std::string resolved{};
  switch (target.kind) {
  case transfer_target_kind::current:
    resolved = std::string{current.name()};
    break;
  case transfer_target_kind::parent:
    if (!current.allows_transfer_to_parent()) {
      return wh::core::result<std::string>::failure(wh::core::errc::contract_violation);
    }
    if (!current.parent_name().has_value()) {
      return wh::core::result<std::string>::failure(wh::core::errc::not_found);
    }
    resolved = std::string{*current.parent_name()};
    break;
  case transfer_target_kind::child:
    if (target.agent_name.empty()) {
      return wh::core::result<std::string>::failure(wh::core::errc::invalid_argument);
    }
    if (!current.allows_transfer_to_child(target.agent_name)) {
      return wh::core::result<std::string>::failure(wh::core::errc::contract_violation);
    }
    if (!current.child(target.agent_name).has_value()) {
      return wh::core::result<std::string>::failure(wh::core::errc::not_found);
    }
    resolved = std::move(target.agent_name);
    break;
  }

  return begin_deterministic_transfer(state, resolved);
}

/// Records one parent-visible message event only when its run path matches the
/// exact bridge-visible path and the payload is not an interrupt control.
inline auto record_parent_visible_event(deterministic_transfer_state &state,
                                        const agent_event &event) -> wh::core::result<void> {
  if (!(event.metadata.path == state.exact_run_path)) {
    return {};
  }
  if (const auto *control = std::get_if<control_action>(&event.payload);
      control != nullptr && control->kind == control_action_kind::interrupt) {
    return {};
  }
  const auto *message = std::get_if<message_event>(&event.payload);
  if (message == nullptr) {
    return {};
  }
  if (const auto *value = std::get_if<wh::schema::message>(&message->content); value != nullptr) {
    state.visible_history.push_back(*value);
    return {};
  }
  return wh::core::result<void>::failure(wh::core::errc::not_supported);
}

/// Rewrites transfer history for one target agent by trimming configured
/// transfer pairs and demoting foreign assistant/tool messages to context text.
[[nodiscard]] inline auto rewrite_transfer_history(
    const std::span<const wh::schema::message> history, const std::string_view current_agent_name,
    const resolved_transfer_trim_options trim = {}) -> std::vector<wh::schema::message> {
  std::vector<wh::schema::message> rewritten{};
  rewritten.reserve(history.size());

  for (std::size_t index = 0U; index < history.size(); ++index) {
    const auto &message = history[index];

    const bool trim_assistant = trim.trim_assistant_transfer_message &&
                                detail::transfer::is_transfer_assistant_message(message);
    const bool trim_tool =
        trim.trim_tool_transfer_pair && detail::transfer::is_transfer_tool_message(message);
    if (trim_assistant || trim_tool) {
      continue;
    }

    const bool foreign_assistant = message.role == wh::schema::message_role::assistant &&
                                   !message.name.empty() && message.name != current_agent_name;
    const bool foreign_tool = message.role == wh::schema::message_role::tool &&
                              !message.name.empty() && message.name != current_agent_name;
    if (foreign_assistant || foreign_tool) {
      rewritten.push_back(
          detail::transfer::make_context_message(detail::transfer::make_context_text(message)));
      continue;
    }

    rewritten.push_back(message);
  }
  return rewritten;
}

/// Appends one transfer assistant/tool message pair exactly once when the
/// transfer completed normally.
inline auto append_transfer_messages_once(std::vector<wh::schema::message> &history,
                                          deterministic_transfer_state &state,
                                          const std::string_view target_agent_name,
                                          const std::string_view tool_call_id,
                                          const transfer_completion_kind completion)
    -> wh::core::result<void> {
  if (completion != transfer_completion_kind::normal) {
    return {};
  }
  if (target_agent_name.empty() || tool_call_id.empty()) {
    return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
  }
  if (state.appended_tool_call_ids.contains(tool_call_id)) {
    return {};
  }

  history.push_back(make_transfer_assistant_message(target_agent_name, tool_call_id));
  history.push_back(make_transfer_tool_message(target_agent_name, tool_call_id));
  state.appended_tool_call_ids.insert(std::string{tool_call_id});
  state.pending_target.reset();
  return {};
}

} // namespace wh::adk
