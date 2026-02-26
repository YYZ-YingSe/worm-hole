#pragma once

#include <span>
#include <typeindex>

#include "wh/core/result.hpp"
#include "wh/internal/merge.hpp"

namespace wh::compose {

[[nodiscard]] inline auto
values_merge(const wh::internal::values_merge_registry &registry,
             const std::type_index type,
             const wh::internal::dynamic_merge_values values)
    -> wh::core::result<wh::internal::dynamic_merge_value> {
  return registry.merge(type, values);
}

template <typename value_t>
[[nodiscard]] inline auto
values_merge(const wh::internal::values_merge_registry &registry,
             const std::span<const value_t> values)
    -> wh::core::result<value_t> {
  return registry.merge_as<value_t>(values);
}

} // namespace wh::compose
