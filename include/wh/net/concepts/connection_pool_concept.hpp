// Defines the compile-time contract required from optional host connection
// pool adapters.
#pragma once

#include <concepts>

#include "wh/net/types.hpp"

namespace wh::net {

/// Requires an optional host connection pool to expose acquire/release
/// operations over the shared connection key/lease protocol.
template <typename pool_t>
concept connection_pool_like =
    requires(const pool_t &pool, const connection_key &key,
             connection_key &&movable_key, const connection_key_view key_view,
             const connection_release &release,
             connection_release &&movable_release) {
      { pool.acquire(key) } -> std::same_as<connection_acquire_result>;
      {
        pool.acquire(std::move(movable_key))
      } -> std::same_as<connection_acquire_result>;
      { pool.acquire(key_view) } -> std::same_as<connection_acquire_result>;
      { pool.release(release) } -> std::same_as<connection_release_result>;
      {
        pool.release(std::move(movable_release))
      } -> std::same_as<connection_release_result>;
    };

} // namespace wh::net
