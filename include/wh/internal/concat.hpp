// Defines internal stream concat registry wrappers on top of the shared
// reducer registry core.
#pragma once

#include <concepts>
#include <cstddef>
#include <span>
#include <type_traits>
#include <utility>

#include "wh/core/error.hpp"
#include "wh/core/error_domain.hpp"
#include "wh/core/result.hpp"
#include "wh/internal/reduce_registry.hpp"

namespace wh::internal {

/// Type-erased stream chunk used by runtime concat registry.
using dynamic_stream_chunk = dynamic_reduce_value;
/// Span view over type-erased stream chunks.
using dynamic_stream_chunks = dynamic_reduce_values;
/// Runtime concat function signature for dynamic chunks.
using dynamic_stream_concat_function = dynamic_reduce_function;

namespace detail {

/// Detects ADL customization point `wh_stream_concat(values)`.
template <typename value_t>
concept adl_stream_concat_available = requires(std::span<const value_t> values) {
  { wh_stream_concat(values) } -> std::same_as<wh::core::result<value_t>>;
};

/// Invokes ADL-provided typed stream concat implementation.
template <typename value_t>
[[nodiscard]] auto static_stream_concat(std::span<const value_t> values)
    -> wh::core::result<value_t> {
  return wh_stream_concat(values);
}

/// Detects maps that support `reserve(size_t)` optimization.
template <typename map_t>
concept concat_reservable_map_like = requires(map_t &map, const std::size_t size) {
  { map.reserve(size) };
};

} // namespace detail

/// Runtime concat registry with typed fast path and dynamic fallback path.
class stream_concat_registry {
public:
  stream_concat_registry() = default;

  /// Reserves dynamic and typed registration tables.
  auto reserve(const std::size_t type_count) -> void { core_.reserve(type_count); }

  /// Freezes registry and rejects future registrations.
  auto freeze() noexcept -> void { core_.freeze(); }

  /// Returns whether registry is frozen.
  [[nodiscard]] auto is_frozen() const noexcept -> bool { return core_.is_frozen(); }

  /// Registers typed concat function and dynamic bridge for `value_t`.
  template <typename value_t, typename function_t>
  auto register_concat(function_t &&function_value) -> wh::core::result<void> {
    return core_.template register_reducer<value_t>(std::forward<function_t>(function_value));
  }

  /// Registers pointer-based typed concat function and dynamic bridge.
  template <typename value_t, typename function_t>
  auto register_concat_from_ptrs(function_t &&function_value) -> wh::core::result<void> {
    return core_.template register_reducer_from_ptrs<value_t>(
        std::forward<function_t>(function_value));
  }

  /// Looks up dynamic concat function by runtime type key.
  [[nodiscard]] auto find_concat(const wh::core::any_type_key type) const noexcept
      -> const dynamic_stream_concat_function * {
    return core_.find_dynamic(type);
  }

  /// Concatenates type-erased chunks by runtime type key.
  [[nodiscard]] auto concat(const wh::core::any_type_key type,
                            const dynamic_stream_chunks values) const
      -> wh::core::result<dynamic_stream_chunk> {
    if (values.empty()) {
      return wh::core::result<dynamic_stream_chunk>::failure(wh::core::errc::invalid_argument);
    }

    if (const auto *function = find_concat(type); function != nullptr) {
      return (*function)(values);
    }

    if (values.size() == 1U) {
      if (values.front().key() != type) {
        return wh::core::result<dynamic_stream_chunk>::failure(wh::core::errc::type_mismatch);
      }
      return values.front();
    }

    return wh::core::result<dynamic_stream_chunk>::failure(wh::core::errc::not_supported);
  }

  /// Concatenates typed chunks using ADL, registered, or single-value fallback.
  template <typename value_t>
  [[nodiscard]] auto concat_as(const std::span<const value_t> values) const
      -> wh::core::result<value_t> {
    if (values.empty()) {
      return wh::core::result<value_t>::failure(wh::core::errc::invalid_argument);
    }

    if constexpr (detail::adl_stream_concat_available<value_t>) {
      return detail::static_stream_concat(values);
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

  /// Number of registered runtime concat handlers.
  [[nodiscard]] auto size() const noexcept -> std::size_t { return core_.size(); }

private:
  reduce_registry_core core_{};
};

} // namespace wh::internal
