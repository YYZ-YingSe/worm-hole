#pragma once

#include <cstddef>

#include "wh/core/small_vector/vector.hpp"

namespace wh::core {

/// Coarse complexity classification for API contracts.
enum class complexity_class {
  /// Constant-time complexity class.
  constant,
  /// Amortized constant-time complexity class.
  amortized_constant,
  /// Linear-time complexity class.
  linear,
};

/// Growth policy snapshot for a small-vector configuration.
struct small_vector_growth_policy {
  /// Minimum heap capacity allocated on first growth.
  std::size_t minimum_dynamic_capacity{8U};
  /// Growth multiplier numerator.
  std::size_t growth_multiplier_num{3U};
  /// Growth multiplier denominator.
  std::size_t growth_multiplier_den{2U};
  /// True when dynamic heap growth is allowed.
  bool heap_enabled{true};
  /// True when shrinking may move storage back to inline buffer.
  bool shrink_to_inline{true};
};

/// Capacity/state snapshot for a specific small-vector instance.
struct small_vector_capacity_snapshot {
  /// Current element count.
  std::size_t size{0U};
  /// Current storage capacity.
  std::size_t capacity{0U};
  /// Compile-time inline capacity.
  std::size_t inline_capacity{0U};
};

/// Behavioral contract summary for a concrete small-vector type.
struct small_vector_contract {
  /// True when inline SBO storage is enabled.
  bool small_object_optimized{true};
  /// True when insertion order is preserved.
  bool preserves_insertion_order{true};
  /// True when conversion to/from `std::vector` is supported.
  bool supports_std_vector_round_trip{true};
  /// True when custom allocator type is supported.
  bool supports_custom_allocator{true};
  /// True when custom options type is supported.
  bool supports_custom_options{true};
  /// Complexity for push-back without triggering growth.
  complexity_class push_back_without_growth{complexity_class::constant};
  /// Complexity for push-back when growth occurs.
  complexity_class push_back_with_growth{complexity_class::amortized_constant};
};

/// Default framework small-vector alias used by generic metadata helpers.
template <typename value_t, std::size_t inline_capacity = 8U,
          typename allocator_t = std::allocator<value_t>,
          typename options_t = small_vector_default_options>
using default_small_vector =
    small_vector<value_t, inline_capacity, allocator_t, options_t>;

/// Returns current size/capacity snapshot.
template <typename value_t, std::size_t inline_capacity, typename allocator_t,
          typename options_t>
[[nodiscard]] auto describe_capacity(
    const small_vector<value_t, inline_capacity, allocator_t, options_t>
        &value) noexcept -> small_vector_capacity_snapshot {
  return small_vector_capacity_snapshot{value.size(), value.capacity(),
                                        inline_capacity};
}

/// Returns compile-time growth policy as runtime metadata.
template <typename value_t, std::size_t inline_capacity, typename allocator_t,
          typename options_t>
[[nodiscard]] constexpr auto describe_growth_policy(
    const small_vector<value_t, inline_capacity, allocator_t, options_t>
        &) noexcept -> small_vector_growth_policy {
  return small_vector_growth_policy{
      options_t::minimum_dynamic_capacity, options_t::growth_numerator,
      options_t::growth_denominator, options_t::heap_enabled,
      options_t::shrink_to_inline};
}

/// Returns behavioral contract for the concrete small-vector type.
template <typename value_t, std::size_t inline_capacity, typename allocator_t,
          typename options_t>
[[nodiscard]] constexpr auto describe_contract(
    const small_vector<value_t, inline_capacity, allocator_t, options_t>
        &) noexcept -> small_vector_contract {
  return small_vector_contract{};
}

} // namespace wh::core
