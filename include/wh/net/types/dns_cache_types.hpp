// Defines DNS-cache request and response types used by the host transport
// abstraction layer.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "wh/net/types/http_client_types.hpp"

namespace wh::net {

/// Borrowed DNS lookup request.
struct dns_lookup_request_view {
  /// Host name to resolve.
  std::string_view host{};
  /// Numeric port associated with the resolution target.
  std::uint16_t port{0U};
};

/// Owned DNS lookup request.
struct dns_lookup_request {
  /// Host name to resolve.
  std::string host{};
  /// Numeric port associated with the resolution target.
  std::uint16_t port{0U};
};

/// One resolved DNS record.
struct dns_record {
  /// Resolved address literal.
  std::string address{};
  /// Numeric port associated with this record.
  std::uint16_t port{0U};
  /// Cache time-to-live exported by the host resolver.
  std::chrono::seconds ttl{0};
};

/// Result payload for one DNS lookup.
struct dns_lookup_response {
  /// Ordered resolved records.
  std::vector<dns_record> records{};
  /// True when the result came from cache.
  bool cache_hit{false};
};

/// Result returned by DNS lookup operations.
using dns_lookup_result = wh::core::result<dns_lookup_response, transport_error>;

/// Result returned by DNS invalidation operations.
using dns_invalidate_result = wh::core::result<void, transport_error>;

/// Projects one owned lookup request into its borrowed view form.
[[nodiscard]] inline auto make_dns_lookup_request_view(const dns_lookup_request &request)
    -> dns_lookup_request_view {
  return dns_lookup_request_view{
      .host = request.host,
      .port = request.port,
  };
}

} // namespace wh::net
