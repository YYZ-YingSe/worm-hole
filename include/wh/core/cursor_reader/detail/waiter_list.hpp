#pragma once

namespace wh::core::cursor_reader_detail {

template <typename waiter_t> class waiter_list {
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
    if (waiter->prev == nullptr && waiter->next == nullptr && head_ != waiter) {
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

  [[nodiscard]] auto front() const noexcept -> waiter_t * { return head_; }
  [[nodiscard]] auto empty() const noexcept -> bool { return head_ == nullptr; }

private:
  waiter_t *head_{nullptr};
  waiter_t *tail_{nullptr};
};

} // namespace wh::core::cursor_reader_detail
