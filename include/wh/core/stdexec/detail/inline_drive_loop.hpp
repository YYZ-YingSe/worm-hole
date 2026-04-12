// Defines the shared inline owner-drive loop used by sender-local state
// machines.
#pragma once

#include <atomic>
#include <concepts>

namespace wh::core::detail {

template <typename derived_t> class inline_drive_loop {
protected:
  auto request_drive() noexcept -> void {
    auto &self = static_cast<derived_t &>(*this);
    if (self.finished() && !completion_pending(self)) {
      return;
    }

    pending_work_.fetch_add(1U, std::memory_order_release);
    bool expected = false;
    if (!driving_.compare_exchange_strong(expected, true,
                                          std::memory_order_acq_rel,
                                          std::memory_order_relaxed)) {
      return;
    }

    drive_until_quiescent(self);
  }

private:
  [[nodiscard]] auto completion_pending(const derived_t &self) const noexcept
      -> bool {
    if constexpr (requires(const derived_t &derived) {
                    { derived.completion_pending() } -> std::convertible_to<bool>;
                  }) {
      return self.completion_pending();
    }
    return false;
  }

  [[nodiscard]] auto complete_completion(derived_t &self) noexcept -> bool {
    if constexpr (requires(derived_t &derived) { derived.take_completion(); }) {
      auto completion = self.take_completion();
      if (!completion.has_value()) {
        return false;
      }
      std::move(*completion).complete();
      return true;
    }
    return false;
  }

  auto drive_until_quiescent(derived_t &self) noexcept -> void {
    for (;;) {
      (void)pending_work_.exchange(0U, std::memory_order_acq_rel);

      if (complete_completion(self)) {
        driving_.store(false, std::memory_order_release);
        return;
      }

      if (!self.finished()) {
        self.drive();
      }

      if (complete_completion(self)) {
        driving_.store(false, std::memory_order_release);
        return;
      }

      if (self.finished()) {
        driving_.store(false, std::memory_order_release);
        return;
      }

      if (pending_work_.load(std::memory_order_acquire) != 0U) {
        continue;
      }

      driving_.store(false, std::memory_order_release);
      if (self.finished() && !completion_pending(self)) {
        return;
      }

      if (pending_work_.load(std::memory_order_acquire) == 0U) {
        return;
      }

      bool expected = false;
      if (!driving_.compare_exchange_strong(expected, true,
                                            std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) {
        return;
      }
    }
  }

  std::atomic<std::uint64_t> pending_work_{0U};
  std::atomic<bool> driving_{false};
};

} // namespace wh::core::detail
