#pragma once

#include <cstddef>

#include "wh/core/small_vector.hpp"

namespace wh::core {

enum class complexity_class {
  constant,
  amortized_constant,
  linear,
};

struct small_vector_growth_policy {
  std::size_t minimum_dynamic_capacity{8U};
  std::size_t growth_multiplier_num{3U};
  std::size_t growth_multiplier_den{2U};
  bool heap_enabled{true};
  bool shrink_to_inline{true};
};

struct small_vector_capacity_snapshot {
  std::size_t size{0U};
  std::size_t capacity{0U};
  std::size_t inline_capacity{0U};
};

struct small_vector_contract {
  bool small_object_optimized{true};
  bool preserves_insertion_order{true};
  bool supports_std_vector_round_trip{true};
  bool supports_custom_allocator{true};
  bool supports_custom_options{true};
  complexity_class push_back_without_growth{complexity_class::constant};
  complexity_class push_back_with_growth{complexity_class::amortized_constant};
};

template <typename value_t, std::size_t inline_capacity = 8U,
          typename allocator_t = std::allocator<value_t>,
          typename options_t = small_vector_default_options>
using default_small_vector =
    small_vector<value_t, inline_capacity, allocator_t, options_t>;

template <typename value_t, std::size_t inline_capacity, typename allocator_t,
          typename options_t>
[[nodiscard]] auto describe_capacity(
    const small_vector<value_t, inline_capacity, allocator_t, options_t>
        &value) noexcept -> small_vector_capacity_snapshot {
  return small_vector_capacity_snapshot{value.size(), value.capacity(),
                                        inline_capacity};
}

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

template <typename value_t, std::size_t inline_capacity, typename allocator_t,
          typename options_t>
[[nodiscard]] constexpr auto describe_contract(
    const small_vector<value_t, inline_capacity, allocator_t, options_t>
        &) noexcept -> small_vector_contract {
  return small_vector_contract{};
}

} // namespace wh::core
