// Defines value-merge composition primitives for combining branch outputs
// into a single resolved value with registered merge strategies.
#pragma once

#include <span>
#include <string_view>

#include "wh/core/result.hpp"
#include "wh/internal/merge.hpp"

namespace wh::compose {

/// Runtime merge path for type-erased values.
[[nodiscard]] inline auto values_merge(const wh::internal::values_merge_registry &registry,
                                       const wh::core::any_type_key type,
                                       const wh::internal::dynamic_merge_values values)
    -> wh::core::result<wh::internal::dynamic_merge_value> {
  return registry.merge(type, values);
}

/// Typed merge path that skips dynamic casts on hot path.
template <typename value_t>
[[nodiscard]] inline auto values_merge(const wh::internal::values_merge_registry &registry,
                                       const std::span<const value_t> values)
    -> wh::core::result<value_t> {
  return registry.merge_as<value_t>(values);
}

} // namespace wh::compose
