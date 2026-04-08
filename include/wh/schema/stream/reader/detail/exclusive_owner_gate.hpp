// Defines a single-owner gate for read-side state machines that need one live
// consumer across idle, topology-wait, round-active, and finishing phases.
#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

namespace wh::schema::stream::detail {

enum class owner_kind : std::uint8_t {
  none = 0U,
  sync,
  async,
};

enum class owner_phase : std::uint8_t {
  idle = 0U,
  topology_wait,
  round_active,
  finishing,
};

class exclusive_owner_gate {
public:
  struct claim {
    std::uint64_t token{0U};
    owner_kind kind{owner_kind::none};
  };

  [[nodiscard]] auto try_claim(const owner_kind kind) noexcept
      -> std::optional<claim> {
    auto current = state_.load(std::memory_order_acquire);
    for (;;) {
      if (decode_kind(current) != owner_kind::none) {
        return std::nullopt;
      }

      const auto next_token = decode_token(current) + 1U;
      const auto desired = encode(next_token, kind, owner_phase::idle);
      if (state_.compare_exchange_weak(current, desired,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
        return claim{
            .token = next_token,
            .kind = kind,
        };
      }
    }
  }

  [[nodiscard]] auto set_phase(const std::uint64_t token,
                               const owner_phase phase) noexcept -> bool {
    auto current = state_.load(std::memory_order_acquire);
    for (;;) {
      if (decode_kind(current) == owner_kind::none ||
          decode_token(current) != token) {
        return false;
      }
      const auto desired =
          encode(token, decode_kind(current), phase);
      if (state_.compare_exchange_weak(current, desired,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
        return true;
      }
    }
  }

  [[nodiscard]] auto release(const std::uint64_t token) noexcept -> bool {
    auto current = state_.load(std::memory_order_acquire);
    for (;;) {
      if (decode_kind(current) == owner_kind::none ||
          decode_token(current) != token) {
        return false;
      }
      const auto desired =
          encode(token, owner_kind::none, owner_phase::idle);
      if (state_.compare_exchange_weak(current, desired,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
        return true;
      }
    }
  }

  [[nodiscard]] auto claimed() const noexcept -> bool {
    return decode_kind(state_.load(std::memory_order_acquire)) !=
           owner_kind::none;
  }

  [[nodiscard]] auto matches(const std::uint64_t token) const noexcept -> bool {
    const auto current = state_.load(std::memory_order_acquire);
    return decode_kind(current) != owner_kind::none &&
           decode_token(current) == token;
  }

  [[nodiscard]] auto current_kind() const noexcept -> owner_kind {
    return decode_kind(state_.load(std::memory_order_acquire));
  }

  [[nodiscard]] auto current_phase() const noexcept -> owner_phase {
    return decode_phase(state_.load(std::memory_order_acquire));
  }

  [[nodiscard]] auto current_token() const noexcept -> std::uint64_t {
    return decode_token(state_.load(std::memory_order_acquire));
  }

private:
  static constexpr std::uint64_t kind_shift_ = 0U;
  static constexpr std::uint64_t phase_shift_ = 8U;
  static constexpr std::uint64_t token_shift_ = 16U;
  static constexpr std::uint64_t byte_mask_ = 0xFFU;

  [[nodiscard]] static constexpr auto encode(const std::uint64_t token,
                                             const owner_kind kind,
                                             const owner_phase phase) noexcept
      -> std::uint64_t {
    return (token << token_shift_) |
           (static_cast<std::uint64_t>(phase) << phase_shift_) |
           (static_cast<std::uint64_t>(kind) << kind_shift_);
  }

  [[nodiscard]] static constexpr auto decode_kind(
      const std::uint64_t value) noexcept -> owner_kind {
    return static_cast<owner_kind>((value >> kind_shift_) & byte_mask_);
  }

  [[nodiscard]] static constexpr auto decode_phase(
      const std::uint64_t value) noexcept -> owner_phase {
    return static_cast<owner_phase>((value >> phase_shift_) & byte_mask_);
  }

  [[nodiscard]] static constexpr auto decode_token(
      const std::uint64_t value) noexcept -> std::uint64_t {
    return value >> token_shift_;
  }

  std::atomic<std::uint64_t> state_{
      encode(0U, owner_kind::none, owner_phase::idle)};
};

} // namespace wh::schema::stream::detail
