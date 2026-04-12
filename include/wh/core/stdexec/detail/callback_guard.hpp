#pragma once

#include <atomic>
#include <cstdint>
#include <utility>

namespace wh::core::detail {

template <typename owner_t> class callback_guard {
public:
  class scope {
  public:
    scope() = default;

    scope(callback_guard *guard, owner_t *owner) noexcept
        : guard_(guard), owner_(owner) {
      guard_->depth_.fetch_add(1U, std::memory_order_acq_rel);
    }

    scope(const scope &) = delete;
    auto operator=(const scope &) -> scope & = delete;

    scope(scope &&other) noexcept
        : guard_(std::exchange(other.guard_, nullptr)),
          owner_(std::exchange(other.owner_, nullptr)) {}

    auto operator=(scope &&other) noexcept -> scope & {
      if (this != &other) {
        release();
        guard_ = std::exchange(other.guard_, nullptr);
        owner_ = std::exchange(other.owner_, nullptr);
      }
      return *this;
    }

    ~scope() { release(); }

  private:
    auto release() noexcept -> void {
      if (guard_ == nullptr) {
        return;
      }
      if (guard_->depth_.fetch_sub(1U, std::memory_order_acq_rel) == 1U) {
        owner_->on_callback_exit();
      }
      guard_ = nullptr;
      owner_ = nullptr;
    }

    callback_guard *guard_{nullptr};
    owner_t *owner_{nullptr};
  };

  [[nodiscard]] auto enter(owner_t *owner) noexcept -> scope {
    return scope{this, owner};
  }

  [[nodiscard]] auto active() const noexcept -> bool {
    return depth_.load(std::memory_order_acquire) != 0U;
  }

private:
  std::atomic<std::uint32_t> depth_{0U};
};

} // namespace wh::core::detail
