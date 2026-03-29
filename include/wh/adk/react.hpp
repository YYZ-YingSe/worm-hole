// Defines ReAct helper functions shared by chat-model-agent wrappers.
#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wh/agent/react.hpp"
#include "wh/agent/toolset.hpp"
#include "wh/model/chat_model.hpp"
#include "wh/core/any.hpp"
#include "wh/core/result.hpp"
#include "wh/schema/message.hpp"

namespace wh::adk {

/// Returns true when one message contains at least one tool-call part.
[[nodiscard]] inline auto
has_tool_call_part(const wh::schema::message &message) noexcept -> bool {
  for (const auto &part : message.parts) {
    if (std::holds_alternative<wh::schema::tool_call_part>(part)) {
      return true;
    }
  }
  return false;
}

/// Renders the plain-text projection of one message for output-key materialization.
[[nodiscard]] inline auto render_message_text(const wh::schema::message &message)
    -> std::string {
  std::string text{};
  for (const auto &part : message.parts) {
    if (const auto *typed = std::get_if<wh::schema::text_part>(&part);
        typed != nullptr) {
      text.append(typed->text);
    }
  }
  return text;
}

/// Builds one system message when agent metadata contributed any instruction
/// text. Empty text produces no extra message.
[[nodiscard]] inline auto make_instruction_message(const std::string_view description,
                                                   const std::string_view instruction)
    -> std::optional<wh::schema::message> {
  std::string text{};
  if (!description.empty()) {
    text.append(description);
  }
  if (!instruction.empty()) {
    if (!text.empty()) {
      text.push_back('\n');
    }
    text.append(instruction);
  }
  if (text.empty()) {
    return std::nullopt;
  }

  wh::schema::message message{};
  message.role = wh::schema::message_role::system;
  message.parts.emplace_back(wh::schema::text_part{std::move(text)});
  return message;
}

/// Extracts ordered tool actions from one assistant message and marks
/// return-direct calls according to the authored toolset.
[[nodiscard]] inline auto collect_tool_actions(
    const wh::schema::message &message, const wh::agent::toolset &tools)
    -> std::vector<wh::agent::react_tool_action> {
  std::vector<wh::agent::react_tool_action> actions{};
  for (const auto &part : message.parts) {
    const auto *tool = std::get_if<wh::schema::tool_call_part>(&part);
    if (tool == nullptr) {
      continue;
    }
    actions.push_back(wh::agent::react_tool_action{
        .call_id = tool->id,
        .tool_name = tool->name,
        .arguments = tool->arguments,
        .return_direct = tools.is_return_direct_tool(tool->name),
    });
  }
  return actions;
}

/// Builds the non-system conversation history forwarded into one full-history
/// tool bridge.
[[nodiscard]] inline auto make_history_request(const wh::agent::react_state &state)
    -> wh::core::result<wh::model::chat_request> {
  wh::model::chat_request request{};
  const auto &messages = state.messages;
  request.messages.reserve(messages.size());
  for (const auto &message : messages) {
    if (message.role == wh::schema::message_role::system) {
      continue;
    }
    request.messages.push_back(message);
  }
  if (!request.messages.empty() &&
      request.messages.back().role == wh::schema::message_role::assistant &&
      has_tool_call_part(request.messages.back())) {
    request.messages.pop_back();
  }
  if (request.messages.empty()) {
    return wh::core::result<wh::model::chat_request>::failure(
        wh::core::errc::not_found);
  }
  return request;
}

/// Converts ReAct tool actions into the compose tools-node input contract.
[[nodiscard]] inline auto
make_tool_batch(const std::span<const wh::agent::react_tool_action> actions)
    -> wh::compose::tool_batch {
  wh::compose::tool_batch batch{};
  batch.calls.reserve(actions.size());
  for (const auto &action : actions) {
    batch.calls.push_back(wh::compose::tool_call{
        .call_id = action.call_id,
        .tool_name = action.tool_name,
        .arguments = action.arguments,
    });
  }
  return batch;
}

/// Converts ReAct tool actions into the compose tools-node input contract and
/// copies one stable history request into every tool call payload.
[[nodiscard]] inline auto make_tool_batch(
    const std::span<const wh::agent::react_tool_action> actions,
    const wh::model::chat_request &history_request) -> wh::compose::tool_batch {
  auto batch = make_tool_batch(actions);
  for (auto &call : batch.calls) {
    call.payload = wh::core::any{history_request};
  }
  return batch;
}

/// Converts one tools-node result payload back into one tool-role message for
/// conversation-state backfill.
[[nodiscard]] inline auto tool_result_to_message(
    const wh::compose::tool_result &result) -> wh::core::result<wh::schema::message> {
  wh::schema::message message{};
  message.role = wh::schema::message_role::tool;
  message.tool_call_id = result.call_id;
  message.tool_name = result.tool_name;

  if (const auto *text = wh::core::any_cast<std::string>(&result.value);
      text != nullptr) {
    message.parts.emplace_back(wh::schema::text_part{*text});
    return message;
  }

  if (const auto *typed_message =
          wh::core::any_cast<wh::schema::message>(&result.value);
      typed_message != nullptr) {
    message = *typed_message;
    if (message.tool_call_id.empty()) {
      message.tool_call_id = result.call_id;
    }
    if (message.tool_name.empty()) {
      message.tool_name = result.tool_name;
    }
    if (message.role != wh::schema::message_role::tool) {
      message.role = wh::schema::message_role::tool;
    }
    return message;
  }

  return wh::core::result<wh::schema::message>::failure(
      wh::core::errc::type_mismatch);
}

/// Materializes the configured output-key payload from the final emitted
/// message without writing into the run-context session bag.
inline auto write_output_value(wh::agent::react_state &state,
                               const std::string_view output_key,
                               const wh::agent::react_output_mode mode,
                               const wh::schema::message &message) -> void {
  if (output_key.empty()) {
    return;
  }
  switch (mode) {
  case wh::agent::react_output_mode::value:
    state.output_values.insert_or_assign(std::string{output_key},
                                         wh::core::any{message});
    return;
  case wh::agent::react_output_mode::stream:
    state.output_values.insert_or_assign(
        std::string{output_key},
        wh::core::any{render_message_text(message)});
    return;
  }
}

} // namespace wh::adk
