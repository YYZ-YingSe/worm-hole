#pragma once

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

#include <stdexec/execution.hpp>

#include "wh/core/compiler.hpp"
#include "wh/core/stdexec/resume_scheduler.hpp"
#include "wh/core/stdexec/scheduler_handoff.hpp"
#include "wh/core/stdexec/try_schedule.hpp"

namespace wh::sync {

namespace detail {

class completion_bits {
public:
  static constexpr std::uint8_t claimed_bit_ = 0x1U;
  static constexpr std::uint8_t completion_started_bit_ = 0x2U;
  static constexpr std::uint8_t payload_ready_bit_ = 0x4U;
  static constexpr std::uint8_t ready_and_claimed_bits_ = payload_ready_bit_ | claimed_bit_;

  [[nodiscard]] auto has_claimed() const noexcept -> bool {
    return (state_bits_.load(std::memory_order_acquire) & claimed_bit_) != 0U;
  }

  [[nodiscard]] auto start_completion() noexcept -> bool {
    return (state_bits_.fetch_or(completion_started_bit_, std::memory_order_acq_rel) &
            completion_started_bit_) == 0U;
  }

  [[nodiscard]] auto mark_ready() noexcept -> std::uint8_t {
    return state_bits_.fetch_or(ready_and_claimed_bits_, std::memory_order_acq_rel);
  }

  [[nodiscard]] static auto is_claimed(const std::uint8_t state_bits) noexcept -> bool {
    return (state_bits & claimed_bit_) != 0U;
  }

private:
  std::atomic<std::uint8_t> state_bits_{0U};
};

} // namespace detail

class async_mutex {
public:
  class lock_guard;

private:
  // State encoding for the lock-free atomic:
  //   not_locked       (1) — mutex is free
  //   locked_no_waiters(0) — mutex held, no pending waiters
  //   pointer value        — mutex held, head of LIFO waiter stack
  static constexpr std::uintptr_t not_locked = 1U;
  static constexpr std::uintptr_t locked_no_waiters = 0U;
  static constexpr std::uint32_t default_spin_count = 16U;

  struct waiter_base {
    waiter_base *next{nullptr};
    void (*complete_fn)(waiter_base *) noexcept {nullptr};
    std::atomic<bool> dequeue_claimed{false};
    bool acquired{false};
  };

  template <typename receiver_t> struct lock_operation;
  class lock_sender;

  // Hot atomic — cache-line aligned to prevent false sharing.
  wh_cacheline_align std::atomic<std::uintptr_t> state_{not_locked};
  // FIFO-reversed list of waiters — accessed only by lock holder.
  waiter_base *waiters_{nullptr};
  std::uint32_t spin_count_{default_spin_count};

  // TTAS: cheap relaxed load first, CAS only when likely to succeed.
  // Reduces cache-coherency traffic under contention.
  [[nodiscard]] auto try_acquire() noexcept -> bool {
    if (state_.load(std::memory_order_relaxed) != not_locked) {
      return false;
    }
    auto expected = not_locked;
    return state_.compare_exchange_strong(expected, locked_no_waiters, std::memory_order_acquire,
                                          std::memory_order_relaxed);
  }

  // Push waiter onto the lock-free stack via CAS loop.
  // Returns true if enqueued (must wait), false if lock acquired.
  [[nodiscard]] auto enqueue(waiter_base &waiter) noexcept -> bool {
    auto old_state = state_.load(std::memory_order_relaxed);
    while (true) {
      if (old_state == not_locked) {
        if (state_.compare_exchange_weak(old_state, locked_no_waiters, std::memory_order_acquire,
                                         std::memory_order_relaxed)) {
          return false;
        }
        continue;
      }
      waiter.next =
          (old_state == locked_no_waiters) ? nullptr : reinterpret_cast<waiter_base *>(old_state);
      if (state_.compare_exchange_weak(old_state, reinterpret_cast<std::uintptr_t>(&waiter),
                                       std::memory_order_release, std::memory_order_relaxed)) {
        return true;
      }
    }
  }

  // Atomically claim a waiter for lock transfer. Returns false if
  // the cancel path already claimed it.
  static auto try_claim_waiter(waiter_base *w) noexcept -> bool {
    bool expected = false;
    return w->dequeue_claimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                      std::memory_order_relaxed);
  }

  auto unlock_one() noexcept -> void {
    for (;;) {
      // Phase 1: drain the FIFO-reversed list (lock-holder private).
      while (waiters_ != nullptr) {
        auto *waiter = waiters_;
        waiters_ = waiter->next;
        waiter->next = nullptr;
        if (try_claim_waiter(waiter)) {
          waiter->acquired = true;
          waiter->complete_fn(waiter);
          return;
        }
        // Cancelled waiter — skip.
      }

      // Phase 2: no FIFO waiters left, try to release the lock.
      auto expected = locked_no_waiters;
      if (state_.compare_exchange_strong(expected, not_locked, std::memory_order_release,
                                         std::memory_order_relaxed)) {
        return;
      }

      // Phase 3: new waiters arrived on the LIFO stack — grab them all
      // and reverse to approximate FIFO.
      auto old_state = state_.exchange(locked_no_waiters, std::memory_order_acquire);
      auto *stack = reinterpret_cast<waiter_base *>(old_state);
      waiter_base *reversed = nullptr;
      while (stack != nullptr) {
        auto *n = stack->next;
        stack->next = reversed;
        reversed = stack;
        stack = n;
      }
      waiters_ = reversed;
      // Loop back to phase 1.
    }
  }

public:
  async_mutex() = default;
  explicit async_mutex(std::uint32_t spin_count) noexcept : spin_count_(spin_count) {}
  ~async_mutex() = default;

  async_mutex(const async_mutex &) = delete;
  auto operator=(const async_mutex &) -> async_mutex & = delete;
  async_mutex(async_mutex &&) = delete;
  auto operator=(async_mutex &&) -> async_mutex & = delete;

  [[nodiscard]] auto lock() noexcept -> lock_sender;
  [[nodiscard]] auto try_lock() noexcept -> std::optional<lock_guard>;
};

class async_mutex::lock_guard {
  async_mutex *mutex_{nullptr};

  friend class async_mutex;
  template <typename> friend struct async_mutex::lock_operation;

  explicit lock_guard(async_mutex *m) noexcept : mutex_(m) {}

public:
  lock_guard() = default;

  ~lock_guard() {
    if (mutex_) {
      mutex_->unlock_one();
    }
  }

  lock_guard(const lock_guard &) = delete;
  auto operator=(const lock_guard &) -> lock_guard & = delete;

  lock_guard(lock_guard &&other) noexcept : mutex_(std::exchange(other.mutex_, nullptr)) {}

  auto operator=(lock_guard &&other) noexcept -> lock_guard & {
    if (this != &other) {
      if (mutex_) {
        mutex_->unlock_one();
      }
      mutex_ = std::exchange(other.mutex_, nullptr);
    }
    return *this;
  }

  void unlock() noexcept {
    if (mutex_) {
      mutex_->unlock_one();
      mutex_ = nullptr;
    }
  }

  [[nodiscard]] explicit operator bool() const noexcept { return mutex_ != nullptr; }
};

inline auto async_mutex::try_lock() noexcept -> std::optional<lock_guard> {
  if (try_acquire()) {
    return lock_guard{this};
  }
  return std::nullopt;
}

template <typename receiver_t> struct async_mutex::lock_operation final : async_mutex::waiter_base {
  using operation_state_concept = stdexec::operation_state_t;

  using stop_token_t = stdexec::stop_token_of_t<stdexec::env_of_t<receiver_t>>;
  using scheduler_t = wh::core::detail::resume_scheduler_t<stdexec::env_of_t<receiver_t>>;

  struct stop_callback;
  struct handoff_receiver;
  struct handoff_value_tag {};
  struct handoff_stopped_tag {};

  using handoff_op_t =
      stdexec::connect_result_t<stdexec::schedule_result_t<scheduler_t>, handoff_receiver>;
  using completion_bits_t = wh::sync::detail::completion_bits;
  using stop_callback_t = stdexec::stop_callback_for_t<stop_token_t, stop_callback>;
  using handoff_completion_t = std::variant<handoff_value_tag, handoff_stopped_tag>;

  async_mutex *mutex{nullptr};
  receiver_t receiver;
  scheduler_t scheduler;
  completion_bits_t completion_bits_{};
  std::optional<stop_callback_t> stop_callback_{};
  // Lazy handoff_op: only constructed when scheduler handoff is needed.
  // Avoids connect(schedule(scheduler), ...) cost on the fast path.
  alignas(handoff_op_t) std::byte handoff_storage_[sizeof(handoff_op_t)];
  bool handoff_constructed_{false};
  std::optional<handoff_completion_t> handoff_completion_{};
  std::atomic<bool> handoff_completion_ready_{false};
  std::atomic<bool> handoff_start_returned_{true};

  auto *handoff_ptr() noexcept {
    return std::launder(reinterpret_cast<handoff_op_t *>(handoff_storage_));
  }

  auto reset_handoff() noexcept -> void {
    if (!handoff_constructed_) {
      return;
    }
    handoff_ptr()->~handoff_op_t();
    handoff_constructed_ = false;
  }

  void construct_handoff() {
    ::new (static_cast<void *>(handoff_storage_))
        handoff_op_t(stdexec::connect(stdexec::schedule(scheduler), handoff_receiver{this}));
    handoff_constructed_ = true;
  }

  [[nodiscard]] auto ensure_handoff() noexcept -> bool {
    if (is_same_scheduler() || handoff_constructed_) {
      return true;
    }
    try {
      construct_handoff();
      return true;
    } catch (...) {
      return false;
    }
  }

  struct stop_callback {
    lock_operation *self{nullptr};
    auto operator()() const noexcept -> void { self->cancel_wait(); }
  };

  struct handoff_receiver {
    using receiver_concept = stdexec::receiver_t;

    lock_operation *self{nullptr};

    auto set_value() noexcept -> void {
      self->publish_handoff_completion(handoff_completion_t{handoff_value_tag{}});
    }

    template <typename error_t> auto set_error(error_t &&) noexcept -> void {
      self->publish_handoff_completion(handoff_completion_t{handoff_stopped_tag{}});
    }

    auto set_stopped() noexcept -> void {
      self->publish_handoff_completion(handoff_completion_t{handoff_stopped_tag{}});
    }

    [[nodiscard]] auto get_env() const noexcept -> stdexec::env<> { return {}; }
  };

  template <typename receiver_value_t>
    requires std::constructible_from<receiver_t, receiver_value_t &&>
  lock_operation(async_mutex *mutex_ptr, receiver_value_t &&receiver_value)
      : mutex(mutex_ptr), receiver(std::forward<receiver_value_t>(receiver_value)),
        scheduler(wh::core::detail::select_resume_scheduler<stdexec::set_value_t>(
            stdexec::get_env(receiver))) {
    this->complete_fn = [](waiter_base *base) noexcept {
      static_cast<lock_operation *>(base)->complete_ready();
    };
  }

  ~lock_operation() { reset_handoff(); }

  lock_operation(const lock_operation &) = delete;
  auto operator=(const lock_operation &) -> lock_operation & = delete;
  lock_operation(lock_operation &&) = delete;
  auto operator=(lock_operation &&) -> lock_operation & = delete;

  [[nodiscard]] auto has_claimed() const noexcept -> bool { return completion_bits_.has_claimed(); }

  [[nodiscard]] auto is_same_scheduler() const noexcept -> bool {
    return wh::core::detail::scheduler_handoff::same_scheduler(scheduler);
  }

  auto release_acquired_lock() noexcept -> void {
    if (!this->acquired || mutex == nullptr) {
      return;
    }
    lock_guard guard{mutex};
    guard.unlock();
    this->acquired = false;
  }

  auto publish_handoff_completion(handoff_completion_t completion) noexcept -> void {
    wh_invariant(!handoff_completion_ready_.load(std::memory_order_acquire));
    handoff_completion_.emplace(std::move(completion));
    handoff_completion_ready_.store(true, std::memory_order_release);
    if (handoff_start_returned_.load(std::memory_order_acquire)) {
      drain_handoff_completion();
    }
  }

  auto drain_handoff_completion() noexcept -> void {
    if (!handoff_completion_ready_.exchange(false, std::memory_order_acq_rel)) {
      return;
    }
    auto completion = std::move(*handoff_completion_);
    handoff_completion_.reset();
    reset_handoff();
    if (std::holds_alternative<handoff_stopped_tag>(completion)) {
      release_acquired_lock();
    }
    complete();
  }

  auto complete_ready() noexcept -> void {
    const auto state_bits = completion_bits_.mark_ready();
    if (completion_bits_t::is_claimed(state_bits)) {
      return;
    }
    if (is_same_scheduler()) {
      complete();
      return;
    }
    wh_invariant(handoff_constructed_);
    try {
      handoff_start_returned_.store(false, std::memory_order_release);
      stdexec::start(*handoff_ptr());
      handoff_start_returned_.store(true, std::memory_order_release);
      if (handoff_completion_ready_.load(std::memory_order_acquire)) {
        drain_handoff_completion();
      }
    } catch (...) {
      handoff_start_returned_.store(true, std::memory_order_release);
      reset_handoff();
      release_acquired_lock();
      complete();
    }
  }

  auto complete() noexcept -> void {
    if (!completion_bits_.start_completion()) {
      return;
    }
    if (this->acquired) {
      stdexec::set_value(std::move(receiver), lock_guard{mutex});
    } else {
      stdexec::set_stopped(std::move(receiver));
    }
  }

  // Fast path: synchronous completion before enqueue — no cancel race
  // possible, so skip completion_bits mark_ready/start_completion atomics.
  auto complete_sync() noexcept -> void {
    if (is_same_scheduler()) {
      stdexec::set_value(std::move(receiver), lock_guard{mutex});
      return;
    }
    // Need scheduler handoff — fall back to normal path.
    this->acquired = true;
    if (!ensure_handoff()) {
      release_acquired_lock();
      stdexec::set_stopped(std::move(receiver));
      return;
    }
    complete_ready();
  }

  auto cancel_wait() noexcept -> void {
    if (mutex == nullptr || has_claimed()) {
      return;
    }
    // Race with unlock_one: first to claim wins.
    if (!try_claim_waiter(this)) {
      return; // unlock already claimed this waiter
    }
    this->acquired = false;
    complete_ready();
  }

  auto prepare_wait(const stop_token_t &stop_token) noexcept -> bool {
    if (!ensure_handoff()) {
      stdexec::set_stopped(std::move(receiver));
      return false;
    }
    if constexpr (!stdexec::unstoppable_token<stop_token_t>) {
      if (!stop_callback_.has_value()) {
        try {
          stop_callback_.emplace(stop_token, stop_callback{this});
        } catch (...) {
          this->acquired = false;
          complete_ready();
          return false;
        }
      }
      if (stop_token.stop_requested()) {
        this->acquired = false;
        complete_ready();
        return false;
      }
    }
    return true;
  }

  auto start() noexcept -> void {
    auto stop_token = stdexec::get_stop_token(stdexec::get_env(receiver));
    if (stop_token.stop_requested()) {
      if (!is_same_scheduler() && !ensure_handoff()) {
        stdexec::set_stopped(std::move(receiver));
        return;
      }
      this->acquired = false;
      complete_ready();
      return;
    }

    // Fast path: synchronous acquisition — skip completion_bits atomics.
    if (mutex->try_acquire()) {
      complete_sync();
      return;
    }

    // TTAS spin: relaxed loads until state looks free, then CAS.
    // Exponential backoff reduces cache-coherency traffic under contention.
    for (std::uint32_t spin = 1U; spin <= mutex->spin_count_; spin <<= 1U) {
      for (std::uint32_t j = 0U; j < spin; ++j) {
        wh::core::spin_pause();
      }
      if (mutex->state_.load(std::memory_order_relaxed) != not_locked) {
        continue;
      }
      if (mutex->try_acquire()) {
        complete_sync();
        return;
      }
    }

    if (!prepare_wait(stop_token)) {
      return;
    }

    // Enqueue returned false means lock was acquired during enqueue.
    const bool pending = mutex->enqueue(*this);
    if (!pending) {
      stop_callback_.reset();
      complete_sync();
      return;
    }

    if constexpr (!stdexec::unstoppable_token<stop_token_t>) {
      if (stop_token.stop_requested()) {
        cancel_wait();
      }
    }
  }
};

class async_mutex::lock_sender {
public:
  using sender_concept = stdexec::sender_t;
  using completion_signatures =
      stdexec::completion_signatures<stdexec::set_value_t(lock_guard), stdexec::set_stopped_t()>;

  explicit lock_sender(async_mutex *mutex_ptr) noexcept : mutex_(mutex_ptr) {}

  template <stdexec::receiver_of<completion_signatures> receiver_t>
    requires wh::core::detail::receiver_with_resume_scheduler<receiver_t>
  [[nodiscard]] auto
  connect(receiver_t receiver) && -> lock_operation<std::remove_cvref_t<receiver_t>> {
    return lock_operation<std::remove_cvref_t<receiver_t>>{mutex_, std::move(receiver)};
  }

  template <stdexec::receiver_of<completion_signatures> receiver_t>
    requires wh::core::detail::receiver_with_resume_scheduler<receiver_t>
  [[nodiscard]] auto
  connect(receiver_t receiver) const & -> lock_operation<std::remove_cvref_t<receiver_t>> {
    return lock_operation<std::remove_cvref_t<receiver_t>>{mutex_, std::move(receiver)};
  }

  template <typename promise_t>
    requires wh::core::detail::promise_with_resume_scheduler<promise_t>
  [[nodiscard]] auto as_awaitable(promise_t &promise) && -> decltype(auto) {
    return stdexec::as_awaitable(make_await_sender(std::move(*this)), promise);
  }

  template <typename promise_t>
    requires wh::core::detail::promise_with_resume_scheduler<promise_t>
  [[nodiscard]] auto as_awaitable(promise_t &promise) const & -> decltype(auto) {
    return stdexec::as_awaitable(make_await_sender(*this), promise);
  }

  [[nodiscard]] auto get_env() const noexcept -> wh::core::detail::async_completion_env {
    return {};
  }

private:
  template <typename self_t> [[nodiscard]] static auto make_await_sender(self_t &&self) {
    return stdexec::then(static_cast<self_t &&>(self),
                         [](lock_guard guard) noexcept -> lock_guard { return guard; });
  }

  async_mutex *mutex_{nullptr};
};

inline auto async_mutex::lock() noexcept -> lock_sender { return lock_sender{this}; }

} // namespace wh::sync
