// Defines multi-producer completion publication with owner-side drain only.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "wh/core/compiler.hpp"

namespace wh::core::detail {

template <typename payload_t> class child_completion_mailbox {
public:
  static constexpr std::uint32_t no_slot_ =
      std::numeric_limits<std::uint32_t>::max();

  struct slot {
    std::optional<payload_t> payload{};
    std::uint32_t next{no_slot_};
  };

  child_completion_mailbox() = default;

  explicit child_completion_mailbox(const std::size_t size) { reset(size); }

  auto reset(const std::size_t size) -> void {
    slots_.clear();
    slots_.resize(size);
    head_.store(no_slot_, std::memory_order_relaxed);
  }

  auto publish(const std::uint32_t slot_id, payload_t payload) noexcept -> bool {
    wh_precondition(slot_id < slots_.size());
    auto &slot = slots_[slot_id];
    if (slot.payload.has_value()) {
      return false;
    }
    slot.payload.emplace(std::move(payload));

    auto head = head_.load(std::memory_order_acquire);
    do {
      slot.next = head;
    } while (!head_.compare_exchange_weak(head, slot_id,
                                          std::memory_order_release,
                                          std::memory_order_acquire));
    return true;
  }

  template <typename fn_t> auto drain(fn_t &&fn) noexcept -> void {
    auto head = head_.exchange(no_slot_, std::memory_order_acquire);
    while (head != no_slot_) {
      const auto slot_id = head;
      wh_invariant(slot_id < slots_.size());
      auto &slot = slots_[slot_id];
      head = slot.next;
      slot.next = no_slot_;
      if (!slot.payload.has_value()) {
        continue;
      }

      auto payload = std::move(*slot.payload);
      slot.payload.reset();
      std::invoke(std::forward<fn_t>(fn), slot_id, std::move(payload));
    }
  }

  [[nodiscard]] auto has_ready() const noexcept -> bool {
    return head_.load(std::memory_order_acquire) != no_slot_;
  }

private:
  std::vector<slot> slots_{};
  std::atomic<std::uint32_t> head_{no_slot_};
};

} // namespace wh::core::detail
