// Defines shared transport errors plus HTTP request/response protocol types
// used by host-provided net adapters.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "wh/core/result.hpp"

namespace wh::net {

/// Normalized transport-error category visible to upper layers.
enum class transport_error_kind {
  /// Unknown transport failure category.
  unknown = 0U,
  /// DNS or endpoint resolution failed.
  resolve,
  /// Connection establishment failed.
  connect,
  /// Request timed out.
  timeout,
  /// Peer or caller canceled the request.
  canceled,
  /// Remote peer returned malformed protocol data.
  protocol,
  /// Authentication or authorization failed.
  auth,
  /// Remote peer rejected the request due to rate limiting.
  rate_limit,
  /// Remote peer or transport dependency is temporarily unavailable.
  unavailable,
};

/// Structured transport error normalized at the net protocol boundary.
struct transport_error {
  /// Canonical transport-error category.
  transport_error_kind kind{transport_error_kind::unknown};
  /// Core error code reused by upper retry/recovery logic.
  wh::core::error_code code{};
  /// True when host policy may safely retry the operation.
  bool retryable{false};
  /// Optional HTTP status code or transport-local numeric status.
  std::optional<std::int32_t> status_code{};
  /// Optional provider/native error code retained for diagnostics.
  std::string provider_code{};
  /// Human-readable error message.
  std::string message{};
  /// Optional structured diagnostic context string.
  std::string diagnostic_context{};
};

/// One HTTP header entry.
struct http_header {
  /// Header field name.
  std::string name{};
  /// Header field value.
  std::string value{};
};

/// Supported HTTP verbs exposed by the abstract protocol.
enum class http_method {
  /// `GET`
  get = 0U,
  /// `POST`
  post,
  /// `PUT`
  put,
  /// `PATCH`
  patch,
  /// `DELETE`
  del,
  /// `HEAD`
  head,
  /// `OPTIONS`
  options,
};

/// Borrowed HTTP request view used when caller controls storage lifetime.
struct http_request_view {
  /// Request method.
  http_method method{http_method::get};
  /// Absolute or host-resolved request URL.
  std::string_view url{};
  /// Borrowed request headers.
  std::span<const http_header> headers{};
  /// Borrowed raw request body.
  std::string_view body{};
};

/// Owned HTTP request payload used for ordinary non-stream requests.
struct http_request {
  /// Request method.
  http_method method{http_method::get};
  /// Absolute or host-resolved request URL.
  std::string url{};
  /// Owned request headers.
  std::vector<http_header> headers{};
  /// Owned raw request body.
  std::string body{};
};

/// Borrowed JSON-request view used by JSON shortcut entrypoints.
struct http_json_request_view {
  /// Request method.
  http_method method{http_method::post};
  /// Absolute or host-resolved request URL.
  std::string_view url{};
  /// Borrowed request headers.
  std::span<const http_header> headers{};
  /// Borrowed JSON body text.
  std::string_view json_body{};
};

/// Owned JSON-request payload used by JSON shortcut entrypoints.
struct http_json_request {
  /// Request method.
  http_method method{http_method::post};
  /// Absolute or host-resolved request URL.
  std::string url{};
  /// Owned request headers.
  std::vector<http_header> headers{};
  /// Owned JSON body text.
  std::string json_body{};
};

/// Supported streaming response protocols.
enum class http_stream_protocol {
  /// Server-sent events.
  sse = 0U,
  /// Opaque raw byte stream.
  raw,
};

/// Borrowed stream-request view used for streaming response paths.
struct http_stream_request_view {
  /// Borrowed base request fields.
  http_request_view request{};
  /// Expected response stream protocol.
  http_stream_protocol protocol{http_stream_protocol::raw};
};

/// Owned stream-request payload used for streaming response paths.
struct http_stream_request {
  /// Owned base request fields.
  http_request request{};
  /// Expected response stream protocol.
  http_stream_protocol protocol{http_stream_protocol::raw};
};

/// Final non-stream HTTP response returned by ordinary invoke paths.
struct http_response {
  /// Final numeric status code.
  std::int32_t status_code{0};
  /// Response headers captured from the host transport.
  std::vector<http_header> headers{};
  /// Fully materialized response body.
  std::string body{};
};

/// Final result for non-stream invoke paths.
using http_invoke_result = wh::core::result<http_response, transport_error>;

/// Projects one owned request into its borrowed view form.
[[nodiscard]] inline auto make_http_request_view(const http_request &request)
    -> http_request_view {
  return http_request_view{
      .method = request.method,
      .url = request.url,
      .headers = std::span<const http_header>{request.headers.data(),
                                              request.headers.size()},
      .body = request.body,
  };
}

/// Projects one owned JSON request into its borrowed view form.
[[nodiscard]] inline auto
make_http_json_request_view(const http_json_request &request)
    -> http_json_request_view {
  return http_json_request_view{
      .method = request.method,
      .url = request.url,
      .headers = std::span<const http_header>{request.headers.data(),
                                              request.headers.size()},
      .json_body = request.json_body,
  };
}

/// Projects one owned stream request into its borrowed view form.
[[nodiscard]] inline auto
make_http_stream_request_view(const http_stream_request &request)
    -> http_stream_request_view {
  return http_stream_request_view{
      .request = make_http_request_view(request.request),
      .protocol = request.protocol,
  };
}

} // namespace wh::net
