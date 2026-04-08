#pragma once

#include <atomic>
#include <cstdint>

namespace wh::core::cursor_reader_detail {

class completion_bits {
public:
  static constexpr std::uint8_t claimed_bit_ = 0x1U;
  static constexpr std::uint8_t delivery_started_bit_ = 0x2U;

  [[nodiscard]] auto has_claimed() const noexcept -> bool {
    return (state_bits_.load(std::memory_order_acquire) & claimed_bit_) != 0U;
  }

  [[nodiscard]] auto claim() noexcept -> bool {
    auto state_bits = state_bits_.load(std::memory_order_acquire);
    for (;;) {
      if ((state_bits & claimed_bit_) != 0U) {
        return false;
      }
      const auto updated = static_cast<std::uint8_t>(state_bits | claimed_bit_);
      if (state_bits_.compare_exchange_weak(state_bits, updated,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
        return true;
      }
    }
  }

  [[nodiscard]] auto start_delivery() noexcept -> bool {
    return (state_bits_.fetch_or(delivery_started_bit_,
                                 std::memory_order_acq_rel) &
            delivery_started_bit_) == 0U;
  }

private:
  std::atomic<std::uint8_t> state_bits_{0U};
};

} // namespace wh::core::cursor_reader_detail
