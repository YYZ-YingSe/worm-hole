// Aggregates net protocol types and the typed host-service bundle used for
// explicit dependency injection.
#pragma once

#include "wh/net/types/connection_pool_types.hpp"
#include "wh/net/types/dns_cache_types.hpp"
#include "wh/net/types/http_client_types.hpp"
#include "wh/net/types/memory_pool_types.hpp"
#include "wh/net/types/sse_parser_types.hpp"

namespace wh::net {

/// Typed bundle of host-provided transport services.
template <typename http_client_t, typename sse_parser_t = std::nullptr_t,
          typename connection_pool_t = std::nullptr_t,
          typename dns_cache_t = std::nullptr_t,
          typename memory_pool_t = std::nullptr_t>
struct transport_services {
  /// Required HTTP client implementation.
  http_client_t *http_client{nullptr};
  /// Optional SSE parser implementation.
  sse_parser_t *sse_parser{nullptr};
  /// Optional connection-pool implementation.
  connection_pool_t *connection_pool{nullptr};
  /// Optional DNS-cache implementation.
  dns_cache_t *dns_cache{nullptr};
  /// Optional memory-pool implementation.
  memory_pool_t *memory_pool{nullptr};
};

} // namespace wh::net
