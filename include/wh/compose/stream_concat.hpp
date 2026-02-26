#pragma once

#include <span>
#include <typeindex>

#include "wh/core/result.hpp"
#include "wh/internal/concat.hpp"

namespace wh::compose {

[[nodiscard]] inline auto
stream_concat(const wh::internal::stream_concat_registry &registry,
              const std::type_index type,
              const wh::internal::dynamic_stream_chunks values)
    -> wh::core::result<wh::internal::dynamic_stream_chunk> {
  return registry.concat(type, values);
}

template <typename value_t>
[[nodiscard]] inline auto
stream_concat(const wh::internal::stream_concat_registry &registry,
              const std::span<const value_t> values)
    -> wh::core::result<value_t> {
  return registry.concat_as<value_t>(values);
}

} // namespace wh::compose
