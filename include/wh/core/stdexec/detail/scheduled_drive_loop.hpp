// Defines the shared scheduler-bound owner-drive loop used by heap-owned
// controller state machines.
#pragma once

#include <atomic>
#include <concepts>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

#include "wh/core/compiler.hpp"
#include "wh/core/error.hpp"
#include "wh/core/error_domain.hpp"
#include "wh/core/stdexec/detail/callback_guard.hpp"
#include "wh/core/stdexec/manual_lifetime_box.hpp"

namespace wh::core::detail {

template <typename error_t>
[[nodiscard]] inline auto
map_scheduled_drive_error(error_t &&error) noexcept -> wh::core::error_code {
  using error_type = std::remove_cvref_t<error_t>;

  if constexpr (std::same_as<error_type, wh::core::error_code>) {
    return std::forward<error_t>(error);
  } else if constexpr (std::same_as<error_type, std::exception_ptr>) {
    try {
      std::rethrow_exception(std::forward<error_t>(error));
    } catch (...) {
      return wh::core::map_current_exception();
    }
  } else {
    return wh::core::errc::internal_error;
  }
}

template <typename derived_t, stdexec::scheduler scheduler_t>
class scheduled_drive_loop {
protected:
  using scheduler_type = std::remove_cvref_t<scheduler_t>;
  friend class wh::core::detail::callback_guard<scheduled_drive_loop>;

  template <typename scheduler_u>
    requires std::constructible_from<scheduler_type, scheduler_u &&>
  explicit scheduled_drive_loop(scheduler_u &&scheduler)
      : scheduler_(std::forward<scheduler_u>(scheduler)) {}

  [[nodiscard]] auto scheduler() const noexcept -> const scheduler_type & {
    return scheduler_;
  }

  ~scheduled_drive_loop() { destroy_all_turns(); }

  auto request_drive() noexcept -> void {
    auto &self = derived();
    if (self.finished() && !completion_pending(self)) {
      return;
    }

    pending_work_.fetch_add(1U, std::memory_order_release);
    bool expected = false;
    if (!drive_claimed_.compare_exchange_strong(expected, true,
                                                std::memory_order_acq_rel,
                                                std::memory_order_relaxed)) {
      return;
    }

    if (!start_new_turn()) {
      return;
    }
    advance_turn_state_machine();
  }

private:
  [[nodiscard]] auto derived() noexcept -> derived_t & {
    return static_cast<derived_t &>(*this);
  }

  struct drive_kick_operation {
    struct receiver {
      using receiver_concept = stdexec::receiver_t;

      drive_kick_operation *self{nullptr};

      auto set_value() && noexcept -> void {
        auto keepalive = self->owner_guard_;
        auto scope = self->owner_->callbacks_.enter(self->owner_);
        self->owner_->run_claimed_drive_body();
        self->owner_->mark_turn_completed(self, std::nullopt);
        static_cast<void>(keepalive);
      }

      template <typename error_t>
      auto set_error(error_t &&error) && noexcept -> void {
        auto keepalive = self->owner_guard_;
        auto scope = self->owner_->callbacks_.enter(self->owner_);
        self->owner_->mark_turn_completed(
            self, map_scheduled_drive_error(std::forward<error_t>(error)));
        static_cast<void>(keepalive);
      }

      auto set_stopped() && noexcept -> void {
        auto keepalive = self->owner_guard_;
        auto scope = self->owner_->callbacks_.enter(self->owner_);
        self->owner_->mark_turn_completed(self, wh::core::errc::canceled);
        static_cast<void>(keepalive);
      }

      [[nodiscard]] auto get_env() const noexcept -> stdexec::env<> {
        return {};
      }
    };

    using sender_type = decltype(stdexec::schedule(
        std::declval<const scheduler_type &>()));
    using op_type = stdexec::connect_result_t<sender_type, receiver>;

    explicit drive_kick_operation(scheduled_drive_loop *owner,
                                  std::shared_ptr<void> owner_guard) noexcept
        : owner_(owner), owner_guard_(std::move(owner_guard)) {}

    scheduled_drive_loop *owner_{nullptr};
    std::shared_ptr<void> owner_guard_{};
    manual_lifetime_box<op_type> op_{};
    std::optional<wh::core::error_code> error_{};
    drive_kick_operation *next_{nullptr};
    bool completed_{false};
    bool start_returned_{false};
  };

  struct settled_turn {
    drive_kick_operation *turn{nullptr};
    std::optional<wh::core::error_code> error{};
  };

  auto start_new_turn() noexcept -> bool {
    std::shared_ptr<void> owner_guard{};
    try {
      owner_guard = make_owner_lifetime_guard();
    } catch (...) {
      fail_claimed_drive(wh::core::map_current_exception());
      return false;
    }

    auto *turn = new drive_kick_operation(this, std::move(owner_guard));
    try {
      auto sender = stdexec::schedule(scheduler_);
      turn->op_.emplace_from(stdexec::connect, std::move(sender),
                             typename drive_kick_operation::receiver{turn});
      {
        std::lock_guard lock{turn_mutex_};
        wh_invariant(current_turn_ == nullptr);
        current_turn_ = turn;
      }

      stdexec::start(turn->op_.get());
      mark_turn_started(turn);
      return true;
    } catch (...) {
      discard_failed_turn(turn);
      fail_claimed_drive(wh::core::map_current_exception());
      return false;
    }
  }

  auto discard_failed_turn(drive_kick_operation *turn) noexcept -> void {
    {
      std::lock_guard lock{turn_mutex_};
      if (current_turn_ == turn) {
        current_turn_ = nullptr;
      }
    }
    turn->owner_guard_.reset();
    delete turn;
  }

  auto mark_turn_started(drive_kick_operation *turn) noexcept -> void {
    std::lock_guard lock{turn_mutex_};
    wh_invariant(current_turn_ == turn);
    turn->start_returned_ = true;
    mature_previous_turns_locked();
  }

  auto mark_turn_completed(
      drive_kick_operation *turn,
      std::optional<wh::core::error_code> error) noexcept -> void {
    std::lock_guard lock{turn_mutex_};
    wh_invariant(current_turn_ == turn);
    wh_invariant(!turn->completed_);
    turn->completed_ = true;
    turn->error_ = std::move(error);
  }

  auto on_callback_exit() noexcept -> void { advance_turn_state_machine(); }

  auto advance_turn_state_machine() noexcept -> void {
    while (!callbacks_.active()) {
      reclaim_reclaimable_turns();

      auto settled = take_settled_current_turn();
      if (!settled.has_value()) {
        return;
      }

      auto *turn = settled->turn;
      turn->owner_guard_.reset();

      if (settled->error.has_value()) {
        derived().drive_error(*settled->error);
        (void)complete_completion(derived());
      }

      auto &self = derived();
      if (self.finished() || completion_pending(self)) {
        enqueue_reclaimable(turn);
        drive_claimed_.store(false, std::memory_order_release);
        reclaim_reclaimable_turns();
        return;
      }

      if (pending_work_.load(std::memory_order_acquire) != 0U) {
        enqueue_awaiting_successor(turn);
        if (!start_new_turn()) {
          return;
        }
        continue;
      }

      park_turn(turn);
      drive_claimed_.store(false, std::memory_order_release);
      if (self.finished() && !completion_pending(self)) {
        reclaim_reclaimable_turns();
        return;
      }
      if (pending_work_.load(std::memory_order_acquire) == 0U) {
        return;
      }

      bool expected = false;
      if (!drive_claimed_.compare_exchange_strong(expected, true,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_relaxed)) {
        return;
      }
      if (!start_new_turn()) {
        return;
      }
    }
  }

  auto mature_previous_turns_locked() noexcept -> void {
    if (parked_turn_ != nullptr) {
      push_turn_locked(reclaimable_turns_, parked_turn_);
      parked_turn_ = nullptr;
    }
    splice_turns_locked(reclaimable_turns_, awaiting_successor_turns_);
  }

  auto push_turn_locked(drive_kick_operation *&head,
                        drive_kick_operation *turn) noexcept -> void {
    turn->next_ = head;
    head = turn;
  }

  auto splice_turns_locked(drive_kick_operation *&dest,
                           drive_kick_operation *&src) noexcept -> void {
    while (src != nullptr) {
      auto *turn = src;
      src = src->next_;
      turn->next_ = dest;
      dest = turn;
    }
  }

  auto enqueue_reclaimable(drive_kick_operation *turn) noexcept -> void {
    std::lock_guard lock{turn_mutex_};
    push_turn_locked(reclaimable_turns_, turn);
  }

  auto enqueue_awaiting_successor(drive_kick_operation *turn) noexcept -> void {
    std::lock_guard lock{turn_mutex_};
    push_turn_locked(awaiting_successor_turns_, turn);
  }

  auto park_turn(drive_kick_operation *turn) noexcept -> void {
    std::lock_guard lock{turn_mutex_};
    wh_invariant(parked_turn_ == nullptr);
    turn->next_ = nullptr;
    parked_turn_ = turn;
  }

  [[nodiscard]] auto take_settled_current_turn() noexcept
      -> std::optional<settled_turn> {
    std::lock_guard lock{turn_mutex_};
    if (current_turn_ == nullptr || !current_turn_->completed_ ||
        !current_turn_->start_returned_) {
      return std::nullopt;
    }

    auto *turn = current_turn_;
    current_turn_ = nullptr;
    turn->next_ = nullptr;
    return settled_turn{
        .turn = turn,
        .error = std::move(turn->error_),
    };
  }

  auto reclaim_reclaimable_turns() noexcept -> void {
    if (callbacks_.active()) {
      return;
    }

    drive_kick_operation *turns = nullptr;
    {
      std::lock_guard lock{turn_mutex_};
      turns = reclaimable_turns_;
      reclaimable_turns_ = nullptr;
    }

    destroy_turn_list(turns);
  }

  auto destroy_turn_list(drive_kick_operation *turns) noexcept -> void {
    while (turns != nullptr) {
      auto *turn = turns;
      turns = turns->next_;
      turn->next_ = nullptr;
      turn->owner_guard_.reset();
      delete turn;
    }
  }

  auto destroy_all_turns() noexcept -> void {
    drive_kick_operation *current = nullptr;
    drive_kick_operation *parked = nullptr;
    drive_kick_operation *awaiting = nullptr;
    drive_kick_operation *reclaimable = nullptr;
    {
      std::lock_guard lock{turn_mutex_};
      current = current_turn_;
      parked = parked_turn_;
      awaiting = awaiting_successor_turns_;
      reclaimable = reclaimable_turns_;
      current_turn_ = nullptr;
      parked_turn_ = nullptr;
      awaiting_successor_turns_ = nullptr;
      reclaimable_turns_ = nullptr;
    }
    destroy_turn_list(current);
    destroy_turn_list(parked);
    destroy_turn_list(awaiting);
    destroy_turn_list(reclaimable);
  }

  auto fail_claimed_drive(const wh::core::error_code error) noexcept -> void {
    drive_claimed_.store(false, std::memory_order_release);
    derived().drive_error(error);
    (void)complete_completion(derived());
  }

  auto run_claimed_drive_body() noexcept -> void {
    auto &self = derived();

    for (;;) {
      (void)pending_work_.exchange(0U, std::memory_order_acq_rel);

      if (complete_completion(self)) {
        return;
      }

      if (!self.finished()) {
        self.drive();
      }

      if (complete_completion(self)) {
        return;
      }

      if (self.finished()) {
        return;
      }

      if (pending_work_.load(std::memory_order_acquire) != 0U) {
        continue;
      }

      return;
    }
  }

  template <typename self_t = derived_t>
  [[nodiscard]] auto make_owner_lifetime_guard_impl(int) noexcept
      -> decltype(std::shared_ptr<void>(
          std::declval<self_t &>().acquire_owner_lifetime_guard())) {
    return static_cast<derived_t *>(this)->acquire_owner_lifetime_guard();
  }

  [[nodiscard]] auto make_owner_lifetime_guard_impl(...) noexcept
      -> std::shared_ptr<void> {
    return {};
  }

  [[nodiscard]] auto make_owner_lifetime_guard() noexcept
      -> std::shared_ptr<void> {
    return make_owner_lifetime_guard_impl(0);
  }

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

  [[no_unique_address]] scheduler_type scheduler_;
  std::atomic<std::uint64_t> pending_work_{0U};
  std::atomic<bool> drive_claimed_{false};
  std::mutex turn_mutex_{};
  drive_kick_operation *current_turn_{nullptr};
  drive_kick_operation *parked_turn_{nullptr};
  drive_kick_operation *awaiting_successor_turns_{nullptr};
  drive_kick_operation *reclaimable_turns_{nullptr};
  wh::core::detail::callback_guard<scheduled_drive_loop> callbacks_{};
};

} // namespace wh::core::detail
