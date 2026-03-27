#pragma once

namespace wh::core::detail {

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
      current->complete(current);
    }
    tail_ = nullptr;
  }

private:
  waiter_t *head_{nullptr};
  waiter_t *tail_{nullptr};
};

} // namespace wh::core::detail
