// Defines the compile-time contract required from host-provided HTTP client
// adapters.
#pragma once

#include <concepts>

#include "wh/net/types.hpp"

namespace wh::net {

/// Requires a host HTTP client to expose ordinary invoke, JSON invoke, and
/// streaming response entrypoints without leaking runtime-specific types.
template <typename client_t>
concept http_client_like = requires(
    const client_t &client, const http_request &request, http_request &&movable_request,
    const http_request_view request_view, const http_json_request &json_request,
    http_json_request &&movable_json_request, const http_json_request_view json_request_view,
    const http_stream_request &stream_request, http_stream_request &&movable_stream_request,
    const http_stream_request_view stream_request_view) {
  { client.invoke(request) } -> std::same_as<http_invoke_result>;
  { client.invoke(std::move(movable_request)) } -> std::same_as<http_invoke_result>;
  { client.invoke(request_view) } -> std::same_as<http_invoke_result>;
  { client.invoke_json(json_request) } -> std::same_as<http_invoke_result>;
  { client.invoke_json(std::move(movable_json_request)) } -> std::same_as<http_invoke_result>;
  { client.invoke_json(json_request_view) } -> std::same_as<http_invoke_result>;
  { client.stream(stream_request) } -> std::same_as<http_stream_result>;
  { client.stream(std::move(movable_stream_request)) } -> std::same_as<http_stream_result>;
  { client.stream(stream_request_view) } -> std::same_as<http_stream_result>;
};

} // namespace wh::net
