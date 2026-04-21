// Defines concept-based forwarding helpers for host-provided HTTP clients
// without introducing a default implementation or type-erased runtime shell.
#pragma once

#include "wh/net/concepts.hpp"

namespace wh::net {

/// Forwards one borrowed ordinary request into a concrete HTTP client.
template <http_client_like client_t>
[[nodiscard]] inline auto invoke_http(const client_t &client, const http_request_view request)
    -> http_invoke_result {
  return client.invoke(request);
}

/// Forwards one owned ordinary request into a concrete HTTP client.
template <http_client_like client_t>
[[nodiscard]] inline auto invoke_http(const client_t &client, const http_request &request)
    -> http_invoke_result {
  return client.invoke(request);
}

/// Forwards one movable ordinary request into a concrete HTTP client.
template <http_client_like client_t>
[[nodiscard]] inline auto invoke_http(const client_t &client, http_request &&request)
    -> http_invoke_result {
  return client.invoke(std::move(request));
}

/// Forwards one borrowed JSON shortcut request into a concrete HTTP client.
template <http_client_like client_t>
[[nodiscard]] inline auto invoke_http_json(const client_t &client,
                                           const http_json_request_view request)
    -> http_invoke_result {
  return client.invoke_json(request);
}

/// Forwards one owned JSON shortcut request into a concrete HTTP client.
template <http_client_like client_t>
[[nodiscard]] inline auto invoke_http_json(const client_t &client, const http_json_request &request)
    -> http_invoke_result {
  return client.invoke_json(request);
}

/// Forwards one movable JSON shortcut request into a concrete HTTP client.
template <http_client_like client_t>
[[nodiscard]] inline auto invoke_http_json(const client_t &client, http_json_request &&request)
    -> http_invoke_result {
  return client.invoke_json(std::move(request));
}

/// Forwards one borrowed streaming request into a concrete HTTP client.
template <http_client_like client_t>
[[nodiscard]] inline auto stream_http(const client_t &client,
                                      const http_stream_request_view request)
    -> http_stream_result {
  return client.stream(request);
}

/// Forwards one owned streaming request into a concrete HTTP client.
template <http_client_like client_t>
[[nodiscard]] inline auto stream_http(const client_t &client, const http_stream_request &request)
    -> http_stream_result {
  return client.stream(request);
}

/// Forwards one movable streaming request into a concrete HTTP client.
template <http_client_like client_t>
[[nodiscard]] inline auto stream_http(const client_t &client, http_stream_request &&request)
    -> http_stream_result {
  return client.stream(std::move(request));
}

} // namespace wh::net
