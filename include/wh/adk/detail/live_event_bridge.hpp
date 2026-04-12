// Defines the internal live event bridge reused by ADK wrappers that need to
// emit events incrementally while retaining checkpoint-safe snapshots.
#pragma once

#include <utility>

#include "wh/adk/event_stream.hpp"

namespace wh::adk::detail {

/// Move-only live event bridge pairing one ADK writer with its reader peer.
struct live_event_bridge {
  /// Writer used by the running wrapper path.
  agent_event_stream_writer writer{};
  /// Reader returned to the caller after the bridge closes.
  agent_event_stream_reader reader{};

  /// Emits one event into the bridge.
  [[nodiscard]] auto emit(agent_event event) -> wh::core::result<void> {
    return send_agent_event(writer, std::move(event));
  }

  /// Closes the write side once the wrapper has finished emitting events.
  [[nodiscard]] auto close() -> wh::core::result<void> {
    return close_agent_event_stream(writer);
  }

  /// Releases the reader side after the write side has been closed.
  [[nodiscard]] auto release_reader() -> agent_event_stream_reader {
    return std::move(reader);
  }
};

/// Creates one live ADK event bridge using the shared schema::stream pipe.
[[nodiscard]] inline auto make_live_event_bridge() -> live_event_bridge {
  auto [writer, reader] = make_agent_event_stream();
  return live_event_bridge{
      .writer = std::move(writer),
      .reader = std::move(reader),
  };
}

} // namespace wh::adk::detail
