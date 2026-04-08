// Defines memory-pool request and block types used by optional host transport
// implementations.
#pragma once

#include <cstddef>
#include <span>

#include "wh/net/types/http_client_types.hpp"

namespace wh::net {

/// Allocation request passed into an optional host memory pool.
struct memory_acquire_request {
  /// Requested byte size.
  std::size_t size_bytes{0U};
  /// Required byte alignment.
  std::size_t alignment{alignof(std::max_align_t)};
};

/// Borrowed memory block returned by the host pool.
struct memory_block {
  /// Borrowed writable byte span.
  std::span<std::byte> bytes{};
};

/// Result returned by memory acquisition.
using memory_acquire_result = wh::core::result<memory_block, transport_error>;

/// Result returned by memory release.
using memory_release_result = wh::core::result<void, transport_error>;

} // namespace wh::net
