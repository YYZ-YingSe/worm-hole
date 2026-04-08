// Defines a single-producer/single-owner completion slot used by serial child
// loops.
#pragma once

#include <atomic>
#include <optional>
#include <utility>

#include "wh/core/compiler.hpp"
#include "wh/core/stdexec/manual_lifetime_box.hpp"

namespace wh::core::detail {

template <typename value_t> class single_completion_slot {
public:
  [[nodiscard]] auto ready() const noexcept -> bool {
    return ready_.load(std::memory_order_acquire);
  }

  auto reset() noexcept -> void {
    if (!ready_.exchange(false, std::memory_order_acq_rel)) {
      return;
    }
    storage_.reset();
  }

  auto publish(value_t value) noexcept -> bool {
    if (ready()) {
      return false;
    }
    try {
      storage_.emplace(std::move(value));
    } catch (...) {
      ::wh::core::contract_violation(
          ::wh::core::contract_kind::invariant,
          "single_completion_slot storage emplacement must not throw");
      return false;
    }
    ready_.store(true, std::memory_order_release);
    return true;
  }

  [[nodiscard]] auto take() noexcept -> std::optional<value_t> {
    if (!ready()) {
      return std::nullopt;
    }
    auto value = std::move(storage_.get());
    storage_.reset();
    ready_.store(false, std::memory_order_release);
    return value;
  }

private:
  wh::core::detail::manual_lifetime_box<value_t> storage_{};
  std::atomic<bool> ready_{false};
};

} // namespace wh::core::detail
