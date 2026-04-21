// Defines internal value merge registry wrappers on top of the shared reducer
// registry core.
#pragma once

#include <concepts>
#include <cstddef>
#include <span>
#include <string>
#include <utility>

#include "wh/core/error.hpp"
#include "wh/core/error_domain.hpp"
#include "wh/core/result.hpp"
#include "wh/internal/reduce_registry.hpp"

namespace wh::internal {

/// Type-erased value used by runtime merge registry.
using dynamic_merge_value = dynamic_reduce_value;
/// Span view over type-erased merge inputs.
using dynamic_merge_values = dynamic_reduce_values;
/// Runtime merge function signature for dynamic values.
using dynamic_values_merge_function = dynamic_reduce_function;

namespace detail {

/// Detects maps that support `reserve(size_t)` optimization.
template <typename map_t>
concept merge_reservable_map_like = requires(map_t &map, const std::size_t size) {
  { map.reserve(size) };
};

} // namespace detail

/// Detects string-keyed map-like containers used by default merge rules.
template <typename map_t>
concept string_keyed_map_like =
    requires(map_t &map, const map_t &const_map, const typename map_t::key_type &key,
             typename map_t::mapped_type value) {
      typename map_t::key_type;
      typename map_t::mapped_type;
      requires std::same_as<std::remove_cv_t<typename map_t::key_type>, std::string>;
      { const_map.find(key) };
      { const_map.end() };
      map.insert_or_assign(key, std::move(value));
    };

/// Runtime merge registry with typed fast path and dynamic fallback path.
class values_merge_registry {
public:
  values_merge_registry() = default;

  /// Reserves dynamic and typed registration tables.
  auto reserve(const std::size_t type_count) -> void { core_.reserve(type_count); }

  /// Freezes registry and rejects future registrations.
  auto freeze() noexcept -> void { core_.freeze(); }

  /// Returns whether registry is frozen.
  [[nodiscard]] auto is_frozen() const noexcept -> bool { return core_.is_frozen(); }

  /// Registers typed merge function and dynamic bridge for `value_t`.
  template <typename value_t, typename function_t>
  auto register_merge(function_t &&function_value) -> wh::core::result<void> {
    return core_.template register_reducer<value_t>(std::forward<function_t>(function_value));
  }

  /// Registers pointer-based typed merge function and dynamic bridge.
  template <typename value_t, typename function_t>
  auto register_merge_from_ptrs(function_t &&function_value) -> wh::core::result<void> {
    return core_.template register_reducer_from_ptrs<value_t>(
        std::forward<function_t>(function_value));
  }

  /// Looks up dynamic merge function by runtime type key.
  [[nodiscard]] auto find_merge(const wh::core::any_type_key type) const noexcept
      -> const dynamic_values_merge_function * {
    return core_.find_dynamic(type);
  }

  /// Merges type-erased values by runtime type key.
  [[nodiscard]] auto merge(const wh::core::any_type_key type,
                           const dynamic_merge_values values) const
      -> wh::core::result<dynamic_merge_value> {
    return core_.reduce(type, values);
  }

  /// Merges typed values with registered or built-in fallback strategies.
  template <typename value_t>
  [[nodiscard]] auto merge_as(const std::span<const value_t> values) const
      -> wh::core::result<value_t> {
    if (values.empty()) {
      return wh::core::result<value_t>::failure(wh::core::errc::invalid_argument);
    }

    if (const auto *typed_function = core_.template find_typed<value_t>();
        typed_function != nullptr) {
      return (*typed_function)(values);
    }

    if (values.size() == 1U) {
      return values.front();
    }

    return wh::core::result<value_t>::failure(wh::core::errc::not_supported);
  }

  /// Number of registered runtime merge handlers.
  [[nodiscard]] auto size() const noexcept -> std::size_t { return core_.size(); }

private:
  reduce_registry_core core_{};
};

} // namespace wh::internal
