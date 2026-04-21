// Defines abstract connection-pool request and lease types used by
// host-provided transport adapters.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "wh/net/types/http_client_types.hpp"

namespace wh::net {

/// Borrowed connection-key view used to address one pooled endpoint.
struct connection_key_view {
  /// Transport scheme such as `http` or `https`.
  std::string_view scheme{};
  /// Logical host name.
  std::string_view host{};
  /// Numeric transport port.
  std::uint16_t port{0U};
};

/// Owned connection-key payload used to address one pooled endpoint.
struct connection_key {
  /// Transport scheme such as `http` or `https`.
  std::string scheme{};
  /// Logical host name.
  std::string host{};
  /// Numeric transport port.
  std::uint16_t port{0U};
};

/// One acquired connection lease returned by the host pool.
struct connection_lease {
  /// Endpoint key associated with this lease.
  connection_key key{};
  /// Optional host-defined lease identifier.
  std::string lease_id{};
  /// True when the lease currently represents a reused live connection.
  bool reused{false};
};

/// Release request passed back into the host pool.
struct connection_release {
  /// Lease being returned.
  connection_lease lease{};
  /// True keeps the connection eligible for reuse.
  bool reusable{true};
};

/// Result returned by connection acquisition.
using connection_acquire_result = wh::core::result<connection_lease, transport_error>;

/// Result returned by connection release.
using connection_release_result = wh::core::result<void, transport_error>;

/// Projects one owned key into its borrowed view form.
[[nodiscard]] inline auto make_connection_key_view(const connection_key &key)
    -> connection_key_view {
  return connection_key_view{
      .scheme = key.scheme,
      .host = key.host,
      .port = key.port,
  };
}

} // namespace wh::net
