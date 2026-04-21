// Defines the compile-time contract required from optional host DNS-cache
// adapters.
#pragma once

#include <concepts>

#include "wh/net/types.hpp"

namespace wh::net {

/// Requires an optional host DNS cache to expose lookup and invalidation
/// operations over the shared request/response protocol.
template <typename cache_t>
concept dns_cache_like =
    requires(const cache_t &cache, const dns_lookup_request &request,
             dns_lookup_request &&movable_request, const dns_lookup_request_view request_view,
             const std::string_view host) {
      { cache.lookup(request) } -> std::same_as<dns_lookup_result>;
      { cache.lookup(std::move(movable_request)) } -> std::same_as<dns_lookup_result>;
      { cache.lookup(request_view) } -> std::same_as<dns_lookup_result>;
      { cache.invalidate(host) } -> std::same_as<dns_invalidate_result>;
    };

} // namespace wh::net
