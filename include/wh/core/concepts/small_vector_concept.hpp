#pragma once

#include <concepts>
#include <cstddef>
#include <vector>

#include "wh/core/small_vector.hpp"

namespace wh::core {

template <typename vector_t>
concept small_vector_like =
    requires(vector_t vector, const vector_t const_vector,
             typename vector_t::value_type value,
             std::vector<typename vector_t::value_type> standard_vector) {
      typename vector_t::value_type;
      typename vector_t::size_type;
      { vector.empty() } -> std::convertible_to<bool>;
      { const_vector.size() } -> std::convertible_to<std::size_t>;
      { const_vector.capacity() } -> std::convertible_to<std::size_t>;
      {
        const_vector.begin()
      } -> std::same_as<typename vector_t::const_iterator>;
      {
        const_vector.cbegin()
      } -> std::same_as<typename vector_t::const_iterator>;
      vector.push_back(value);
      vector.insert(vector.begin(), value);
      vector.erase(vector.begin());
      {
        const_vector.to_std_vector()
      } -> std::same_as<std::vector<typename vector_t::value_type>>;
      { vector_t::from_std_vector(standard_vector) };
    };

static_assert(small_vector_like<small_vector<int>>);

} // namespace wh::core
