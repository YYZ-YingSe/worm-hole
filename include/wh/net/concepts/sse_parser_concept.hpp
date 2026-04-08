// Defines the compile-time contract required from host-provided SSE parser
// adapters.
#pragma once

#include <concepts>

#include "wh/net/types.hpp"

namespace wh::net {

/// Requires an incremental SSE parser to accept owned and borrowed chunks and
/// return normalized parse output.
template <typename parser_t>
concept sse_parser_like =
    requires(const parser_t &parser, const sse_parse_request &request,
             sse_parse_request &&movable_request,
             const sse_parse_request_view request_view) {
      { parser.parse(request) } -> std::same_as<sse_parse_result>;
      {
        parser.parse(std::move(movable_request))
      } -> std::same_as<sse_parse_result>;
      { parser.parse(request_view) } -> std::same_as<sse_parse_result>;
    };

} // namespace wh::net
