// Defines fixed-slot ready publication for multi-producer / single-owner
// operation states.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

#include "wh/core/compiler.hpp"

namespace wh::core::detail {

class slot_ready_list {
public:
  static constexpr std::uint32_t no_slot = std::numeric_limits<std::uint32_t>::max();

  slot_ready_list() = default;

  explicit slot_ready_list(const std::size_t size) { reset(size); }

  slot_ready_list(const slot_ready_list &) = delete;
  auto operator=(const slot_ready_list &) -> slot_ready_list & = delete;
  slot_ready_list(slot_ready_list &&) = delete;
  auto operator=(slot_ready_list &&) -> slot_ready_list & = delete;

  auto reset(const std::size_t size) -> void {
    if (size > max_slot_capacity()) {
      throw std::length_error{"slot_ready_list size exceeds supported uint32_t slot capacity"};
    }
    slots_ = size == 0U ? nullptr : std::make_unique<slot[]>(size);
    size_ = size;
    head_.store(no_slot, std::memory_order_relaxed);
  }

  auto publish(const std::uint32_t slot_id) noexcept -> bool {
    wh_precondition(slot_id < size_);
    auto &slot_state = slots_[slot_id];
    if (slot_state.queued.exchange(true, std::memory_order_acq_rel)) {
      return false;
    }

    auto head = head_.load(std::memory_order_acquire);
    do {
      slot_state.next = head;
    } while (!head_.compare_exchange_weak(head, slot_id, std::memory_order_release,
                                          std::memory_order_acquire));
    return true;
  }

  template <typename fn_t> auto drain(fn_t &&fn) noexcept -> void {
    auto head = head_.exchange(no_slot, std::memory_order_acquire);
    while (head != no_slot) {
      const auto slot_id = head;
      wh_invariant(slot_id < size_);
      auto &slot_state = slots_[slot_id];
      head = slot_state.next;
      slot_state.next = no_slot;
      slot_state.queued.store(false, std::memory_order_release);
      std::invoke(std::forward<fn_t>(fn), slot_id);
    }
  }

  [[nodiscard]] auto has_ready() const noexcept -> bool {
    return head_.load(std::memory_order_acquire) != no_slot;
  }

private:
  struct slot {
    std::atomic<bool> queued{false};
    std::uint32_t next{no_slot};
  };

  [[nodiscard]] static constexpr auto max_slot_capacity() noexcept -> std::size_t {
    return std::min(static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()),
                    static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max() /
                                             static_cast<std::ptrdiff_t>(sizeof(slot))));
  }

  std::unique_ptr<slot[]> slots_{};
  std::size_t size_{0U};
  std::atomic<std::uint32_t> head_{no_slot};
};

} // namespace wh::core::detail
