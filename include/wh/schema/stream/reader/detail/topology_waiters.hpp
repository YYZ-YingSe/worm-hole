// Defines shared intrusive waiter utilities and topology wait-notification
// primitives used by merge topology registries.
#pragma once

#include <atomic>
#include <cstdint>

namespace detail {

template <typename waiter_t> class intrusive_waiter_list {
public:
  auto push_back(waiter_t *waiter) noexcept -> void {
    waiter->prev = tail_;
    waiter->next = nullptr;
    if (tail_ != nullptr) {
      tail_->next = waiter;
    } else {
      head_ = waiter;
    }
    tail_ = waiter;
  }

  [[nodiscard]] auto try_pop_front() noexcept -> waiter_t * {
    if (head_ == nullptr) {
      return nullptr;
    }
    auto *waiter = head_;
    head_ = head_->next;
    if (head_ != nullptr) {
      head_->prev = nullptr;
    } else {
      tail_ = nullptr;
    }
    waiter->prev = nullptr;
    waiter->next = nullptr;
    return waiter;
  }

  [[nodiscard]] auto try_remove(waiter_t *waiter) noexcept -> bool {
    if (waiter == nullptr) {
      return false;
    }
    if (waiter->prev == nullptr && waiter->next == nullptr &&
        head_ != waiter) {
      return false;
    }

    auto *previous = waiter->prev;
    auto *next = waiter->next;
    if (previous != nullptr) {
      previous->next = next;
    } else {
      head_ = next;
    }
    if (next != nullptr) {
      next->prev = previous;
    } else {
      tail_ = previous;
    }
    waiter->prev = nullptr;
    waiter->next = nullptr;
    return true;
  }

private:
  waiter_t *head_{nullptr};
  waiter_t *tail_{nullptr};
};

template <typename waiter_t> struct waiter_ops {
  void (*complete)(waiter_t *) noexcept {nullptr};
};

template <typename waiter_t> class waiter_ready_list {
public:
  auto push_back(waiter_t *waiter) noexcept -> void {
    waiter->next = nullptr;
    waiter->prev = nullptr;
    if (tail_ != nullptr) {
      tail_->next = waiter;
    } else {
      head_ = waiter;
    }
    tail_ = waiter;
  }

  auto complete_all() noexcept -> void {
    while (head_ != nullptr) {
      auto *current = head_;
      head_ = head_->next;
      current->next = nullptr;
      current->prev = nullptr;
      current->ops->complete(current);
    }
    tail_ = nullptr;
  }

private:
  waiter_t *head_{nullptr};
  waiter_t *tail_{nullptr};
};

struct topology_sync_waiter {
  topology_sync_waiter *next{nullptr};
  topology_sync_waiter *prev{nullptr};
  std::atomic_flag ready = ATOMIC_FLAG_INIT;

  auto notify() noexcept -> void {
    ready.test_and_set(std::memory_order_release);
    ready.notify_one();
  }

  auto wait() noexcept -> void {
    while (!ready.test(std::memory_order_acquire)) {
      ready.wait(false, std::memory_order_acquire);
    }
  }
};

struct topology_async_waiter {
  using ops_type = waiter_ops<topology_async_waiter>;

  topology_async_waiter *next{nullptr};
  topology_async_waiter *prev{nullptr};
  const ops_type *ops{nullptr};
};

enum class topology_poll_mode : std::uint8_t {
  fixed = 0U,
  dynamic,
};

} // namespace detail
