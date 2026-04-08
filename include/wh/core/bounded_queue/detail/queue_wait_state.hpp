#pragma once

#include <optional>

#include "wh/core/bounded_queue/detail/waiter_list.hpp"

namespace wh::core::detail {

template <typename push_waiter_t, typename pop_waiter_t = push_waiter_t>
class wait_state {
public:
  struct detached_waiters {
    push_waiter_t *push_head{nullptr};
    pop_waiter_t *pop_head{nullptr};
  };

  [[nodiscard]] auto is_closed() const noexcept -> bool { return closed_; }

  [[nodiscard]] auto close() noexcept -> bool {
    if (closed_) {
      return false;
    }
    closed_ = true;
    return true;
  }

  [[nodiscard]] auto close_and_detach() noexcept
      -> std::optional<detached_waiters> {
    if (closed_) {
      return std::nullopt;
    }
    closed_ = true;
    return detached_waiters{push_waiters_.detach_all(),
                            pop_waiters_.detach_all()};
  }

  auto enqueue_push(push_waiter_t *waiter) noexcept -> void {
    push_waiters_.push_back(waiter);
  }

  auto enqueue_pop(pop_waiter_t *waiter) noexcept -> void {
    pop_waiters_.push_back(waiter);
  }

  [[nodiscard]] auto remove_push(push_waiter_t *waiter) noexcept -> bool {
    return push_waiters_.try_remove(waiter);
  }

  [[nodiscard]] auto remove_pop(pop_waiter_t *waiter) noexcept -> bool {
    return pop_waiters_.try_remove(waiter);
  }

  [[nodiscard]] auto front_push() const noexcept -> push_waiter_t * {
    return push_waiters_.front();
  }

  [[nodiscard]] auto front_pop() const noexcept -> pop_waiter_t * {
    return pop_waiters_.front();
  }

  [[nodiscard]] auto take_push() noexcept -> push_waiter_t * {
    return push_waiters_.try_pop_front();
  }

  [[nodiscard]] auto take_pop() noexcept -> pop_waiter_t * {
    return pop_waiters_.try_pop_front();
  }

private:
  waiter_list<push_waiter_t> push_waiters_{};
  waiter_list<pop_waiter_t> pop_waiters_{};
  bool closed_{false};
};

} // namespace wh::core::detail
