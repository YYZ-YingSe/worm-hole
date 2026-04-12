// Defines ADK event-stream aliases and helpers on top of existing
// schema::stream primitives.
#pragma once

#include <concepts>
#include <exception>
#include <functional>
#include <utility>

#include "wh/adk/types.hpp"
#include "wh/core/error.hpp"
#include "wh/core/error_domain.hpp"
#include "wh/core/result.hpp"
#include "wh/schema/stream/core/types.hpp"
#include "wh/schema/stream/pipe.hpp"

namespace wh::adk {

/// Type-erased reader used by ADK event streams.
using agent_event_stream_reader =
    wh::schema::stream::any_stream_reader<agent_event>;

/// Type-erased writer used by ADK event streams.
using agent_event_stream_writer =
    wh::schema::stream::any_stream_writer<agent_event>;

/// Owned chunk shell carried by one ADK event-stream read.
using agent_event_stream_chunk = wh::schema::stream::stream_chunk<agent_event>;

/// Blocking read result for one ADK event-stream read.
using agent_event_stream_result =
    wh::schema::stream::stream_result<agent_event_stream_chunk>;

/// Non-blocking read result for one ADK event-stream read.
using agent_event_stream_try_result =
    wh::schema::stream::stream_try_result<agent_event_stream_chunk>;

/// Reads one ADK event chunk and normalizes an unbound reader to EOF.
[[nodiscard]] inline auto
read_agent_event_stream(agent_event_stream_reader &reader)
    -> agent_event_stream_result {
  auto next_chunk = reader.read();
  if (next_chunk.has_error() &&
      next_chunk.error() == wh::core::errc::not_found) {
    return agent_event_stream_chunk::make_eof();
  }
  return next_chunk;
}

/// Tries to read one ADK event chunk and normalizes an unbound reader to EOF.
[[nodiscard]] inline auto
try_read_agent_event_stream(agent_event_stream_reader &reader)
    -> agent_event_stream_try_result {
  auto next_chunk = reader.try_read();
  if (auto *result = std::get_if<agent_event_stream_result>(&next_chunk);
      result != nullptr && result->has_error() &&
      result->error() == wh::core::errc::not_found) {
    return agent_event_stream_result{agent_event_stream_chunk::make_eof()};
  }
  return next_chunk;
}

/// Closes one ADK event reader and treats an unbound reader as already closed.
[[nodiscard]] inline auto
close_agent_event_stream(agent_event_stream_reader &reader)
    -> wh::core::result<void> {
  auto closed = reader.close();
  if (closed.has_error() && closed.error() == wh::core::errc::not_found) {
    return {};
  }
  return closed;
}

/// Sends one ADK event without leaking pipe-specific full or not-found errors.
[[nodiscard]] inline auto send_agent_event(agent_event_stream_writer &writer,
                                           agent_event &&event)
    -> wh::core::result<void> {
  auto status = writer.try_write(std::move(event));
  if (status.has_error() && status.error() == wh::core::errc::queue_full) {
    return wh::core::result<void>::failure(wh::core::errc::resource_exhausted);
  }
  if (status.has_error() && status.error() == wh::core::errc::not_found) {
    return wh::core::result<void>::failure(wh::core::errc::channel_closed);
  }
  return status;
}

/// Closes one ADK event writer and treats an unbound writer as already closed.
[[nodiscard]] inline auto
close_agent_event_stream(agent_event_stream_writer &writer)
    -> wh::core::result<void> {
  auto closed = writer.close();
  if (closed.has_error() && closed.error() == wh::core::errc::not_found) {
    return {};
  }
  return closed;
}

/// Creates one ADK event-stream writer and reader pair.
[[nodiscard]] inline auto make_agent_event_stream()
    -> std::pair<agent_event_stream_writer, agent_event_stream_reader> {
  auto [writer, reader] =
      wh::schema::stream::make_pipe_stream<agent_event>(64U);
  return {agent_event_stream_writer{std::move(writer)},
          agent_event_stream_reader{std::move(reader)}};
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
    return send_agent_event(writer,
                            std::invoke(std::forward<factory_t>(factory)));
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
