// Defines ADK event-stream bridge helpers that reuse the shared event-stream
// aliases instead of introducing a second transport layer.
#pragma once

#include <concepts>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "wh/adk/event_stream.hpp"
#include "wh/core/error_domain.hpp"
#include "wh/schema/message.hpp"
#include "wh/schema/stream/algorithm.hpp"

namespace wh::adk {

/// Shared run output shape used by workflow/bridge/template wrappers.
struct agent_run_output {
  /// Event stream produced by the wrapped agent execution.
  agent_event_stream_reader events{};
  /// Final message observed on the wrapped execution path when available.
  std::optional<wh::schema::message> final_message{};
  /// Output values explicitly materialized by the wrapped execution.
  std::unordered_map<std::string, wh::core::any, wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      output_values{};
};

/// Canonical success/failure boundary for one wrapped agent execution.
using agent_run_result = wh::core::result<agent_run_output>;

/// Concatenates two run paths without mutating either source path.
[[nodiscard]] inline auto append_run_path_prefix(const run_path &prefix,
                                                 const run_path &suffix)
    -> run_path {
  run_path combined = prefix;
  for (const auto &segment : suffix.segments()) {
    combined = combined.append(segment);
  }
  return combined;
}

/// Prefixes one event run path with the supplied parent path.
[[nodiscard]] inline auto prefix_agent_event(agent_event event,
                                             const run_path &prefix)
    -> agent_event {
  event.metadata.run_path =
      append_run_path_prefix(prefix, event.metadata.run_path);
  return event;
}

/// Collects one message reader into owned messages.
[[nodiscard]] inline auto
collect_agent_messages(agent_message_stream_reader &&reader)
    -> wh::core::result<std::vector<wh::schema::message>> {
  return wh::schema::stream::collect_stream_reader(std::move(reader));
}

/// Collects one event reader into owned events.
[[nodiscard]] inline auto
collect_agent_events(agent_event_stream_reader &&reader)
    -> wh::core::result<std::vector<agent_event>> {
  return wh::schema::stream::collect_stream_reader(std::move(reader));
}

/// Snapshots one movable message event into owned messages.
[[nodiscard]] inline auto snapshot_message_event(message_event event)
    -> wh::core::result<std::vector<wh::schema::message>> {
  if (const auto *value = std::get_if<wh::schema::message>(&event.content);
      value != nullptr) {
    return std::vector<wh::schema::message>{*value};
  }
  if (auto *stream = std::get_if<agent_message_stream_reader>(&event.content);
      stream != nullptr) {
    return collect_agent_messages(std::move(*stream));
  }
  return wh::core::result<std::vector<wh::schema::message>>::failure(
      wh::core::errc::type_mismatch);
}

/// Returns the last owned message carried by one event sequence, when any.
[[nodiscard]] inline auto
find_final_message(const std::vector<agent_event> &events)
    -> std::optional<wh::schema::message> {
  for (auto iter = events.rbegin(); iter != events.rend(); ++iter) {
    const auto *message = std::get_if<message_event>(&iter->payload);
    if (message == nullptr) {
      continue;
    }
    if (const auto *value = std::get_if<wh::schema::message>(&message->content);
        value != nullptr) {
      return *value;
    }
  }
  return std::nullopt;
}

/// Executes `factory` and sends either the produced event or a structured
/// error event derived from the thrown exception.
template <typename factory_t>
  requires std::invocable<factory_t>
inline auto send_agent_event_or_error(agent_event_stream_writer &writer,
                                      factory_t &&factory,
                                      event_metadata metadata = {})
    -> wh::core::result<void> {
  try {
    return send_agent_event(writer, std::invoke(std::forward<factory_t>(factory)));
  } catch (const std::exception &error) {
    return send_agent_event(
        writer, make_error_event(wh::core::map_exception(error), error.what(),
                                 {}, std::move(metadata)));
  } catch (...) {
    return send_agent_event(
        writer,
        make_error_event(wh::core::make_error(wh::core::errc::internal_error),
                         "unknown", {}, std::move(metadata)));
  }
}

} // namespace wh::adk
