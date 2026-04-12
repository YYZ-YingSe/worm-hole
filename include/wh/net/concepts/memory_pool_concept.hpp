// Defines the compile-time contract required from optional host memory-pool
// adapters.
#pragma once

#include <concepts>

#include "wh/net/types.hpp"

namespace wh::net {

/// Requires an optional host memory pool to expose acquire/release operations
/// over the shared memory-block protocol.
template <typename pool_t>
concept memory_pool_like =
    requires(const pool_t &pool, const memory_acquire_request &request,
             memory_acquire_request &&movable_request,
             const memory_block &block, memory_block &&movable_block) {
      { pool.acquire(request) } -> std::same_as<memory_acquire_result>;
      {
        pool.acquire(std::move(movable_request))
      } -> std::same_as<memory_acquire_result>;
      { pool.release(block) } -> std::same_as<memory_release_result>;
      {
        pool.release(std::move(movable_block))
      } -> std::same_as<memory_release_result>;
    };

} // namespace wh::net
