// Defines a small scheduled turn controller for owner-managed resume loops.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <exec/trampoline_scheduler.hpp>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

#include "wh/core/compiler.hpp"
#include "wh/core/error.hpp"
#include "wh/core/error_domain.hpp"
namespace wh::core::detail {

template <typename owner_t, stdexec::scheduler scheduler_t>
class scheduled_resume_turn {
  using scheduler_type = std::remove_cvref_t<scheduler_t>;

  struct receiver {
    using receiver_concept = stdexec::receiver_t;

    scheduled_resume_turn *turn{nullptr};
    owner_t *owner{nullptr};

    auto set_value() && noexcept -> void {
      auto *turn_ptr = turn;
      auto *owner_ptr = owner;
      turn_ptr->begin();
      turn_ptr->run_loop(owner_ptr);
      turn_ptr->finish(owner_ptr);
      owner_ptr->resume_turn_arrive();
    }

    template <typename error_t> auto set_error(error_t &&error) && noexcept -> void {
      auto *turn_ptr = turn;
      auto *owner_ptr = owner;
      turn_ptr->begin();
      owner_ptr->resume_turn_schedule_error(
          map_error(std::forward<error_t>(error)));
      turn_ptr->run_loop(owner_ptr);
      turn_ptr->finish(owner_ptr);
      owner_ptr->resume_turn_arrive();
    }

    auto set_stopped() && noexcept -> void {
      auto *turn_ptr = turn;
      auto *owner_ptr = owner;
      turn_ptr->begin();
      owner_ptr->resume_turn_schedule_error(wh::core::errc::canceled);
      turn_ptr->run_loop(owner_ptr);
      turn_ptr->finish(owner_ptr);
      owner_ptr->resume_turn_arrive();
    }

    [[nodiscard]] auto get_env() const noexcept -> stdexec::env<> { return {}; }
  };

  using sender_t =
      decltype(stdexec::starts_on(exec::trampoline_scheduler{},
                                  stdexec::schedule(
                                      std::declval<const scheduler_type &>())));
  using op_t = stdexec::connect_result_t<sender_t, receiver>;

public:
  template <typename scheduler_u>
    requires std::constructible_from<scheduler_type, scheduler_u &&>
  explicit scheduled_resume_turn(scheduler_u &&scheduler)
      : scheduler_(std::forward<scheduler_u>(scheduler)) {}

  scheduled_resume_turn(const scheduled_resume_turn &) = delete;
  auto operator=(const scheduled_resume_turn &) -> scheduled_resume_turn & = delete;
  scheduled_resume_turn(scheduled_resume_turn &&) = delete;
  auto operator=(scheduled_resume_turn &&) -> scheduled_resume_turn & = delete;

  ~scheduled_resume_turn() { destroy(); }

  [[nodiscard]] auto running() const noexcept -> bool { return running_; }

  auto request(owner_t *owner) noexcept -> void {
    if (owner->resume_turn_completed()) {
      return;
    }

    pending_work_.fetch_add(1U, std::memory_order_release);
    bool start_turn = false;
    bool run_inline = false;
    std::optional<wh::core::error_code> start_error{};
    {
      std::lock_guard lock{mutex_};
      if (owner->resume_turn_completed() || running_ || op_engaged_) {
        return;
      }

      start_turn = advance_locked(owner, run_inline, start_error);
    }

    if (run_inline) {
      owner->resume_turn_schedule_error(*start_error);
      run_loop(owner);
      finish(owner);
      return;
    }
    if (start_turn) {
      start(owner);
    }
  }

  auto destroy() noexcept -> void {
    std::lock_guard lock{mutex_};
    if (!op_engaged_) {
      return;
    }
    wh_invariant(!running_);
    op()->~op_t();
    op_engaged_ = false;
    start_returned_ = false;
    turn_completed_ = false;
  }

private:
  [[nodiscard]] auto op() noexcept -> op_t * {
    return std::launder(reinterpret_cast<op_t *>(op_storage_));
  }

  [[nodiscard]] auto op() const noexcept -> const op_t * {
    return std::launder(reinterpret_cast<const op_t *>(op_storage_));
  }

  template <typename error_t>
  [[nodiscard]] static auto map_error(error_t &&error) noexcept
      -> wh::core::error_code {
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
      return wh::core::make_error(wh::core::errc::internal_error);
    }
  }

  auto begin() noexcept -> void {
    std::lock_guard lock{mutex_};
    wh_invariant(op_engaged_);
    wh_invariant(!running_);
    running_ = true;
  }

  auto run_loop(owner_t *owner) noexcept -> void {
    while (!owner->resume_turn_completed()) {
      (void)pending_work_.exchange(0U, std::memory_order_acq_rel);
      owner->resume_turn_run();
      if (owner->resume_turn_completed()) {
        return;
      }
      if (pending_work_.load(std::memory_order_acquire) == 0U) {
        owner->resume_turn_idle();
        return;
      }
    }
  }

  auto finish(owner_t *owner) noexcept -> void {
    bool start_turn = false;
    bool run_inline = false;
    std::optional<wh::core::error_code> start_error{};
    {
      std::lock_guard lock{mutex_};
      wh_invariant(running_);
      running_ = false;
      turn_completed_ = true;
      start_turn = advance_locked(owner, run_inline, start_error);
    }

    if (run_inline) {
      owner->resume_turn_schedule_error(*start_error);
      run_loop(owner);
      finish(owner);
      return;
    }
    if (start_turn) {
      start(owner);
    }
  }

  auto arm_locked(owner_t *owner) -> void {
    if (op_engaged_) {
      return;
    }
    ::new (static_cast<void *>(op()))
        op_t(stdexec::connect(stdexec::starts_on(exec::trampoline_scheduler{},
                                                stdexec::schedule(scheduler_)),
                              receiver{this, owner}));
    op_engaged_ = true;
    start_returned_ = false;
    turn_completed_ = false;
    owner->resume_turn_add_ref();
  }

  auto start(owner_t *owner) noexcept -> void {
    stdexec::start(*op());
    mark_started(owner);
  }

  auto mark_started(owner_t *owner) noexcept -> void {
    bool start_turn = false;
    bool run_inline = false;
    std::optional<wh::core::error_code> start_error{};
    {
      std::lock_guard lock{mutex_};
      wh_invariant(op_engaged_);
      start_returned_ = true;
      start_turn = advance_locked(owner, run_inline, start_error);
    }

    if (run_inline) {
      owner->resume_turn_schedule_error(*start_error);
      run_loop(owner);
      finish(owner);
      return;
    }
    if (start_turn) {
      start(owner);
    }
  }

  auto reset_completed_locked() noexcept -> void {
    wh_invariant(op_engaged_);
    wh_invariant(turn_completed_);
    wh_invariant(start_returned_);
    wh_invariant(!running_);
    op()->~op_t();
    op_engaged_ = false;
    start_returned_ = false;
    turn_completed_ = false;
  }

  [[nodiscard]] auto advance_locked(
      owner_t *owner, bool &run_inline,
      std::optional<wh::core::error_code> &start_error) -> bool {
    if (op_engaged_ && turn_completed_ && start_returned_ && !running_) {
      reset_completed_locked();
    }

    if (owner->resume_turn_completed() || running_ || op_engaged_ ||
        pending_work_.load(std::memory_order_acquire) == 0U) {
      return false;
    }

    try {
      arm_locked(owner);
      return true;
    } catch (...) {
      start_error = wh::core::map_current_exception();
      running_ = true;
      run_inline = true;
      return false;
    }
  }

  scheduler_type scheduler_;
  alignas(op_t) std::byte op_storage_[sizeof(op_t)];
  std::mutex mutex_{};
  std::atomic<std::uint64_t> pending_work_{0U};
  bool op_engaged_{false};
  bool running_{false};
  bool start_returned_{false};
  bool turn_completed_{false};
};

} // namespace wh::core::detail
