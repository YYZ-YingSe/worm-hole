// Defines shared history-request helpers reused by agent-tool and ReAct
// lowerings.
#pragma once

#include <optional>
#include <utility>

#include "wh/agent/react.hpp"
#include "wh/core/any.hpp"
#include "wh/core/result.hpp"
#include "wh/model/chat_model.hpp"

namespace wh::adk::detail {

[[nodiscard]] inline auto
history_request_has_tool_call_part(const wh::schema::message &message) noexcept
    -> bool {
  for (const auto &part : message.parts) {
    if (std::holds_alternative<wh::schema::tool_call_part>(part)) {
      return true;
    }
  }
  return false;
}

/// Structured tool-call payload carrying the rewritten history request plus
/// one optional bridge-local state snapshot.
struct history_request_payload {
  /// Rewritten history request visible to history-aware tool bridges.
  wh::model::chat_request history_request{};
  /// Optional typed bridge-local state snapshot retained by the caller.
  wh::core::any state_payload{};
};

/// Borrowed view over one normalized history-request payload boundary.
struct history_request_payload_view {
  /// Rewritten history request forwarded into one history-aware bridge.
  const wh::model::chat_request *history_request{nullptr};
  /// Optional bridge-local state snapshot preserved next to the request.
  const wh::core::any *state_payload{nullptr};
};

/// Builds the non-system conversation history forwarded into one full-history
/// tool bridge.
[[nodiscard]] inline auto make_history_request(const wh::agent::react_state &state)
    -> wh::core::result<wh::model::chat_request> {
  wh::model::chat_request request{};
  const auto &messages = state.messages;
  request.messages.reserve(messages.size());
  std::optional<std::size_t> trailing_tool_call_index{};
  for (std::size_t index = messages.size(); index > 0U; --index) {
    const auto &message = messages[index - 1U];
    if (message.role == wh::schema::message_role::system) {
      continue;
    }
    if (message.role == wh::schema::message_role::assistant &&
        history_request_has_tool_call_part(message)) {
      trailing_tool_call_index = index - 1U;
    }
    break;
  }

  for (std::size_t index = 0U; index < messages.size(); ++index) {
    const auto &message = messages[index];
    if (message.role == wh::schema::message_role::system) {
      continue;
    }
    if (trailing_tool_call_index.has_value() &&
        *trailing_tool_call_index == index) {
      continue;
    }
    auto rewritten = wh::agent::rewrite_history_message_as_context_prompt(message);
    if (!rewritten.has_value()) {
      continue;
    }
    request.messages.push_back(std::move(*rewritten));
  }
  if (request.messages.empty()) {
    return wh::core::result<wh::model::chat_request>::failure(
        wh::core::errc::not_found);
  }
  return request;
}

/// Reads one shared history-request payload from a tool-call boundary payload.
[[nodiscard]] inline auto
read_history_request_payload_view(const wh::core::any &payload)
    -> wh::core::result<history_request_payload_view> {
  if (!payload.has_value()) {
    return wh::core::result<history_request_payload_view>::failure(
        wh::core::errc::not_found);
  }
  if (const auto *request =
          wh::core::any_cast<wh::model::chat_request>(&payload);
      request != nullptr) {
    return history_request_payload_view{
        .history_request = request,
    };
  }
  if (const auto *typed =
          wh::core::any_cast<history_request_payload>(&payload);
      typed != nullptr) {
    return history_request_payload_view{
        .history_request = &typed->history_request,
        .state_payload = &typed->state_payload,
    };
  }
  return wh::core::result<history_request_payload_view>::failure(
      wh::core::errc::type_mismatch);
}

/// Reads one shared history-request payload from a tool-call boundary payload.
[[nodiscard]] inline auto read_history_request_payload(const wh::core::any &payload)
    -> wh::core::result<history_request_payload> {
  auto view = read_history_request_payload_view(payload);
  if (view.has_error()) {
    return wh::core::result<history_request_payload>::failure(view.error());
  }

  history_request_payload normalized{};
  normalized.history_request = *view.value().history_request;
  if (view.value().state_payload != nullptr) {
    auto owned = wh::core::into_owned(*view.value().state_payload);
    if (owned.has_error()) {
      return wh::core::result<history_request_payload>::failure(owned.error());
    }
    normalized.state_payload = std::move(owned).value();
  }
  return normalized;
}

} // namespace wh::adk::detail

namespace wh::core {

template <> struct any_owned_traits<wh::adk::detail::history_request_payload> {
  [[nodiscard]] static auto into_owned(const wh::adk::detail::history_request_payload &value)
      -> wh::core::result<wh::adk::detail::history_request_payload> {
    auto state_payload = wh::core::into_owned(value.state_payload);
    if (state_payload.has_error()) {
      return wh::core::result<wh::adk::detail::history_request_payload>::failure(
          state_payload.error());
    }
    return wh::adk::detail::history_request_payload{
        .history_request = value.history_request,
        .state_payload = std::move(state_payload).value(),
    };
  }

  [[nodiscard]] static auto into_owned(wh::adk::detail::history_request_payload &&value)
      -> wh::core::result<wh::adk::detail::history_request_payload> {
    auto state_payload = wh::core::into_owned(std::move(value.state_payload));
    if (state_payload.has_error()) {
      return wh::core::result<wh::adk::detail::history_request_payload>::failure(
          state_payload.error());
    }
    return wh::adk::detail::history_request_payload{
        .history_request = std::move(value.history_request),
        .state_payload = std::move(state_payload).value(),
    };
  }
};

} // namespace wh::core
