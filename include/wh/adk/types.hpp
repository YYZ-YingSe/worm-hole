// Defines ADK event, message, and control protocol types on top of existing
// schema and core capabilities.
#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <variant>

#include "wh/core/address.hpp"
#include "wh/core/any.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/schema/message.hpp"
#include "wh/schema/serialization/registry.hpp"
#include "wh/schema/stream/core/any_stream.hpp"

namespace wh::adk {

/// Stable run-path alias shared across ADK authoring and diagnostics.
using run_path = wh::core::address;

/// Type-erased single-consumer reader used by ADK message-stream events.
using agent_message_stream_reader = wh::schema::stream::any_stream_reader<wh::schema::message>;

/// Single-message or message-stream payload carried by one ADK message event.
using message_content = std::variant<wh::schema::message, agent_message_stream_reader>;

/// Message-bearing event payload.
struct message_event {
  /// Message value or single-consumer message stream carried by the event.
  message_content content{};
};

/// Control actions propagated across ADK scopes.
enum class control_action_kind {
  /// Stop current authored path and return normally.
  exit = 0U,
  /// Transfer execution to another named agent.
  transfer,
  /// Surface an interrupt to the caller or resume boundary.
  interrupt,
  /// Break the current authored loop body.
  break_loop,
};

/// Control action payload carried by one ADK event.
struct control_action {
  /// Action kind propagated across authored boundaries.
  control_action_kind kind{control_action_kind::exit};
  /// Optional transfer target name for `transfer`.
  std::string target{};
  /// Optional interrupt identifier for `interrupt`.
  std::string interrupt_id{};
  /// Optional human-readable reason for diagnostics.
  std::string reason{};
};

/// Custom business event payload emitted through the ADK event stream.
struct custom_event {
  /// Stable custom event name.
  std::string name{};
  /// Type-erased event payload owned by the event shell.
  wh::core::any payload{};
};

/// Structured error payload emitted through the ADK event stream.
struct error_event {
  /// Canonical framework error code.
  wh::core::error_code code{};
  /// Human-readable error message retained for diagnostics.
  std::string message{};
  /// Optional typed detail payload checked at checkpoint boundaries.
  wh::core::any detail{};
};

/// Extra metadata attached to every ADK event.
struct event_metadata {
  /// Stable run-path snapshot for this event.
  run_path path{};
  /// Agent name that emitted the event.
  std::string agent_name{};
  /// Tool name associated with the event, if any.
  std::string tool_name{};
  /// Arbitrary metadata fields attached by bridges or governance wrappers.
  std::unordered_map<std::string, wh::core::any, wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      attributes{};
};

/// Canonical ADK event payload variant.
using agent_event_payload = std::variant<message_event, control_action, custom_event, error_event>;

/// Canonical ADK event shell.
struct agent_event {
  /// Payload carried by this event.
  agent_event_payload payload{};
  /// Metadata attached to the payload shell.
  event_metadata metadata{};
};

/// Builds a message event from one owned message value.
[[nodiscard]] inline auto make_message_event(wh::schema::message message,
                                             event_metadata metadata = {}) -> agent_event {
  return agent_event{
      .payload = message_event{.content = std::move(message)},
      .metadata = std::move(metadata),
  };
}

/// Builds a message event from one message-stream reader.
[[nodiscard]] inline auto make_message_event(agent_message_stream_reader stream,
                                             event_metadata metadata = {}) -> agent_event {
  return agent_event{
      .payload = message_event{.content = std::move(stream)},
      .metadata = std::move(metadata),
  };
}

/// Builds a control event from one control action.
[[nodiscard]] inline auto make_control_event(control_action action, event_metadata metadata = {})
    -> agent_event {
  return agent_event{
      .payload = std::move(action),
      .metadata = std::move(metadata),
  };
}

/// Builds a custom event from one type-erased payload.
[[nodiscard]] inline auto make_custom_event(std::string name, wh::core::any payload,
                                            event_metadata metadata = {}) -> agent_event {
  return agent_event{
      .payload =
          custom_event{
              .name = std::move(name),
              .payload = std::move(payload),
          },
      .metadata = std::move(metadata),
  };
}

/// Builds an error event with optional typed detail payload.
[[nodiscard]] inline auto make_error_event(wh::core::error_code code, std::string message,
                                           wh::core::any detail = {}, event_metadata metadata = {})
    -> agent_event {
  return agent_event{
      .payload =
          error_event{
              .code = code,
              .message = std::move(message),
              .detail = std::move(detail),
          },
      .metadata = std::move(metadata),
  };
}

namespace detail {

[[nodiscard]] inline auto
validate_registered_payload(const wh::core::any &payload,
                            const wh::schema::serialization_registry &registry)
    -> wh::core::result<void> {
  if (!payload.has_value()) {
    return {};
  }

  const auto name = registry.primary_name_for_key(payload.info().key);
  if (name.has_error()) {
    return wh::core::result<void>::failure(wh::core::errc::serialize_error);
  }
  return {};
}

} // namespace detail

/// Validates whether one event is safe to cross a checkpoint serialization
/// boundary without inventing a second serialization protocol.
[[nodiscard]] inline auto
validate_agent_event_checkpoint_serializable(const agent_event &event,
                                             const wh::schema::serialization_registry &registry)
    -> wh::core::result<void> {
  if (const auto *message = std::get_if<message_event>(&event.payload); message != nullptr) {
    if (std::holds_alternative<agent_message_stream_reader>(message->content)) {
      return wh::core::result<void>::failure(wh::core::errc::serialize_error);
    }
    return {};
  }

  if (const auto *custom = std::get_if<custom_event>(&event.payload); custom != nullptr) {
    return detail::validate_registered_payload(custom->payload, registry);
  }

  if (const auto *error = std::get_if<error_event>(&event.payload); error != nullptr) {
    return detail::validate_registered_payload(error->detail, registry);
  }

  return {};
}

} // namespace wh::adk

namespace wh::core {

template <> struct any_owned_traits<wh::adk::event_metadata> {
  [[nodiscard]] static auto into_owned(const wh::adk::event_metadata &value)
      -> wh::core::result<wh::adk::event_metadata> {
    auto attributes = wh::core::into_owned_any_map(value.attributes);
    if (attributes.has_error()) {
      return wh::core::result<wh::adk::event_metadata>::failure(attributes.error());
    }
    return wh::adk::event_metadata{
        .path = value.path,
        .agent_name = value.agent_name,
        .tool_name = value.tool_name,
        .attributes = std::move(attributes).value(),
    };
  }

  [[nodiscard]] static auto into_owned(wh::adk::event_metadata &&value)
      -> wh::core::result<wh::adk::event_metadata> {
    auto attributes = wh::core::into_owned_any_map(std::move(value.attributes));
    if (attributes.has_error()) {
      return wh::core::result<wh::adk::event_metadata>::failure(attributes.error());
    }
    return wh::adk::event_metadata{
        .path = std::move(value.path),
        .agent_name = std::move(value.agent_name),
        .tool_name = std::move(value.tool_name),
        .attributes = std::move(attributes).value(),
    };
  }
};

} // namespace wh::core
