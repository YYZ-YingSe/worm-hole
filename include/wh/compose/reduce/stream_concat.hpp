// Defines stream concatenation composition primitives for merging multiple
// upstream streams into one ordered output stream.
#pragma once

#include <span>
#include <string_view>

#include "wh/core/result.hpp"
#include "wh/internal/concat.hpp"

namespace wh::compose {

/// Session key carrying optional fan-in stream-concat registry pointer.
inline constexpr std::string_view graph_stream_concat_registry_session_key =
    "compose.graph.fan_in.stream_concat_registry";

/// Runtime concat path for type-erased stream chunks.
[[nodiscard]] inline auto
stream_concat(const wh::internal::stream_concat_registry &registry,
              const wh::core::any_type_key type,
              const wh::internal::dynamic_stream_chunks values)
    -> wh::core::result<wh::internal::dynamic_stream_chunk> {
  return registry.concat(type, values);
}

template <typename value_t>
/// Typed concat path that avoids dynamic casting on hot path.
[[nodiscard]] inline auto
stream_concat(const wh::internal::stream_concat_registry &registry,
              const std::span<const value_t> values)
    -> wh::core::result<value_t> {
  return registry.concat_as<value_t>(values);
}

} // namespace wh::compose
