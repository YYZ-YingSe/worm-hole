// Defines SSE and raw-stream protocol types reused by the abstract HTTP and
// parser concepts.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "wh/net/types/http_client_types.hpp"
#include "wh/schema/stream/core/any_stream.hpp"

namespace wh::net {

/// One parsed SSE event.
struct sse_event {
  /// Optional SSE event name.
  std::string event{};
  /// Optional SSE event id.
  std::string id{};
  /// Concatenated SSE `data:` payload.
  std::string data{};
  /// Optional retry hint expressed in milliseconds.
  std::optional<std::uint64_t> retry_millis{};
};

/// One opaque raw-stream chunk.
struct raw_stream_chunk {
  /// Raw byte payload emitted by the host stream.
  std::vector<std::byte> bytes{};
};

/// One logical stream item surfaced to upper layers.
struct http_stream_event {
  /// Protocol that produced this item.
  http_stream_protocol protocol{http_stream_protocol::raw};
  /// Parsed SSE event or raw byte chunk.
  std::variant<sse_event, raw_stream_chunk> payload{};
};

/// Reader used by streaming HTTP responses.
using http_stream_reader =
    wh::schema::stream::any_stream_reader<http_stream_event>;

/// Final result for streaming invoke paths.
using http_stream_result =
    wh::core::result<http_stream_reader, transport_error>;

/// Borrowed parser input view for one incremental SSE parse step.
struct sse_parse_request_view {
  /// Borrowed incoming bytes.
  std::span<const std::byte> bytes{};
  /// True marks the current chunk as end-of-stream flush.
  bool end_of_stream{false};
};

/// Owned parser input for one incremental SSE parse step.
struct sse_parse_request {
  /// Owned incoming bytes.
  std::vector<std::byte> bytes{};
  /// True marks the current chunk as end-of-stream flush.
  bool end_of_stream{false};
};

/// Parsed SSE output produced by one parse step.
struct sse_parse_output {
  /// Fully parsed SSE events produced from the supplied chunk.
  std::vector<sse_event> events{};
  /// True when parser reached a terminal flushed state.
  bool end_of_stream{false};
};

/// Result type returned by SSE parser implementations.
using sse_parse_result = wh::core::result<sse_parse_output, transport_error>;

/// Projects one owned parse request into its borrowed view form.
[[nodiscard]] inline auto
make_sse_parse_request_view(const sse_parse_request &request)
    -> sse_parse_request_view {
  return sse_parse_request_view{
      .bytes = std::span<const std::byte>{request.bytes.data(),
                                          request.bytes.size()},
      .end_of_stream = request.end_of_stream,
  };
}

} // namespace wh::net
