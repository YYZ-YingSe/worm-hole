// Defines one compact runtime-sized bitset for compose graph internals.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "wh/core/compiler.hpp"

namespace wh::compose::detail {

struct dynamic_bitset {
  static constexpr std::size_t bits_per_word = 64U;

  dynamic_bitset() = default;

  explicit dynamic_bitset(const std::size_t bit_count,
                          const bool fill = false) {
    reset(bit_count, fill);
  }

  auto reset(const std::size_t bit_count, const bool fill = false) -> void {
    bit_count_ = bit_count;
    const auto word_count = (bit_count + bits_per_word - 1U) / bits_per_word;
    words_.resize(word_count);
    std::fill(words_.begin(), words_.end(),
              fill ? ~std::uint64_t{0U} : std::uint64_t{0U});
    if (fill && !words_.empty() && (bit_count % bits_per_word) != 0U) {
      const auto used_bits = bit_count % bits_per_word;
      const auto mask = (std::uint64_t{1U} << used_bits) - 1U;
      words_.back() &= mask;
    }
  }

  [[nodiscard]] auto test(const std::size_t index) const noexcept -> bool {
    wh_precondition(index < bit_count_);
    const auto word = index / bits_per_word;
    const auto bit = index % bits_per_word;
    return (words_[word] & (std::uint64_t{1U} << bit)) != 0U;
  }

  auto set(const std::size_t index) noexcept -> void {
    wh_precondition(index < bit_count_);
    const auto word = index / bits_per_word;
    const auto bit = index % bits_per_word;
    words_[word] |= (std::uint64_t{1U} << bit);
  }

  auto clear(const std::size_t index) noexcept -> void {
    wh_precondition(index < bit_count_);
    const auto word = index / bits_per_word;
    const auto bit = index % bits_per_word;
    words_[word] &= ~(std::uint64_t{1U} << bit);
  }

  [[nodiscard]] auto set_if_unset(const std::size_t index) noexcept -> bool {
    wh_precondition(index < bit_count_);
    const auto word = index / bits_per_word;
    const auto bit = index % bits_per_word;
    const auto mask = (std::uint64_t{1U} << bit);
    if ((words_[word] & mask) != 0U) {
      return false;
    }
    words_[word] |= mask;
    return true;
  }

  [[nodiscard]] auto size() const noexcept -> std::size_t { return bit_count_; }

  auto swap(dynamic_bitset &other) noexcept -> void {
    words_.swap(other.words_);
    std::swap(bit_count_, other.bit_count_);
  }

private:
  std::vector<std::uint64_t> words_{};
  std::size_t bit_count_{0U};
};

} // namespace wh::compose::detail
