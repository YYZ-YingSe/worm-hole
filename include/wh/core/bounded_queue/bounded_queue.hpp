#pragma once

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

#include <stdexec/execution.hpp>

#include "wh/core/bounded_queue/detail/critical_section.hpp"
#include "wh/core/bounded_queue/detail/queue_wait_state.hpp"
#include "wh/core/bounded_queue/detail/ring_storage.hpp"
#include "wh/core/bounded_queue/detail/waiter.hpp"
#include "wh/core/bounded_queue/status.hpp"
#include "wh/core/compiler.hpp"
#include "wh/core/result.hpp"
#include "wh/core/stdexec.hpp"

namespace wh::core {

namespace detail {

struct would_block {};

template <typename scheduler_t, typename receiver_t,
          bool enabled = wh::core::try_scheduler<scheduler_t>>
struct try_handoff_op_selector {
  using type = std::monostate;
};

template <typename scheduler_t, typename receiver_t>
struct try_handoff_op_selector<scheduler_t, receiver_t, true> {
  using type = stdexec::connect_result_t<
      wh::core::detail::scheduler_handoff::try_schedule_handoff_sender<scheduler_t>, receiver_t>;
};

template <typename scheduler_t, typename receiver_t>
using try_handoff_op_t = typename try_handoff_op_selector<scheduler_t, receiver_t>::type;

template <typename scheduler_t, typename receiver_t,
          bool enabled = wh::core::try_scheduler<scheduler_t>>
class try_handoff_storage {
public:
  using state_t = try_handoff_op_t<scheduler_t, receiver_t>;

  template <typename factory_t> auto ensure_handoff(factory_t &&factory) noexcept -> bool {
    if (!state_.has_value()) {
      try {
        state_.emplace(std::forward<factory_t>(factory)());
      } catch (...) {
        return false;
      }
    }
    return true;
  }

  auto reset() noexcept -> void { state_.reset(); }

  [[nodiscard]] auto state() noexcept -> state_t & { return *state_; }

private:
  std::optional<state_t> state_{};
};

template <typename scheduler_t, typename receiver_t>
class try_handoff_storage<scheduler_t, receiver_t, false> {
public:
  template <typename factory_t> auto ensure_handoff(factory_t &&) noexcept -> bool { return false; }

  auto reset() noexcept -> void {}
};

class completion_bits {
public:
  static constexpr std::uint8_t claimed_bit_ = 0x1U;
  static constexpr std::uint8_t delivery_started_bit_ = 0x2U;
  static constexpr std::uint8_t payload_ready_bit_ = 0x4U;
  static constexpr std::uint8_t try_arrived_bit_ = 0x8U;
  static constexpr std::uint8_t try_blocked_bit_ = 0x10U;
  static constexpr std::uint8_t ready_and_claimed_bits_ = payload_ready_bit_ | claimed_bit_;
  static constexpr std::uint8_t try_reset_bits_ =
      try_arrived_bit_ | try_blocked_bit_ | delivery_started_bit_;

  [[nodiscard]] auto has_claimed() const noexcept -> bool {
    return (state_bits_.load(std::memory_order_acquire) & claimed_bit_) != 0U;
  }

  auto release_claim() noexcept -> void {
    state_bits_.fetch_and(static_cast<std::uint8_t>(~claimed_bit_), std::memory_order_release);
  }

  [[nodiscard]] auto start_delivery() noexcept -> bool {
    return (state_bits_.fetch_or(delivery_started_bit_, std::memory_order_acq_rel) &
            delivery_started_bit_) == 0U;
  }

  [[nodiscard]] auto mark_ready() noexcept -> std::uint8_t {
    return state_bits_.fetch_or(ready_and_claimed_bits_, std::memory_order_acq_rel);
  }

  [[nodiscard]] auto claim_try_handoff() noexcept -> bool {
    auto state_bits = state_bits_.load(std::memory_order_acquire);
    for (;;) {
      if ((state_bits & claimed_bit_) != 0U) {
        return false;
      }
      const auto updated = static_cast<std::uint8_t>(
          (state_bits & static_cast<std::uint8_t>(~try_reset_bits_)) | claimed_bit_);
      if (state_bits_.compare_exchange_weak(state_bits, updated, std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
        return true;
      }
    }
  }

  [[nodiscard]] auto mark_try_arrived() noexcept -> std::uint8_t {
    return static_cast<std::uint8_t>(
        state_bits_.fetch_or(try_arrived_bit_, std::memory_order_release) | try_arrived_bit_);
  }

  [[nodiscard]] auto mark_try_blocked() noexcept -> std::uint8_t {
    return static_cast<std::uint8_t>(
        state_bits_.fetch_or(try_blocked_bit_, std::memory_order_release) | try_blocked_bit_);
  }

  auto reset_try_handoff() noexcept -> void {
    state_bits_.fetch_and(
        static_cast<std::uint8_t>(~(payload_ready_bit_ | try_arrived_bit_ | try_blocked_bit_)),
        std::memory_order_release);
  }

  [[nodiscard]] auto try_blocked() const noexcept -> bool {
    return (state_bits_.load(std::memory_order_acquire) & try_blocked_bit_) != 0U;
  }

  [[nodiscard]] static auto ready(const std::uint8_t state_bits) noexcept -> bool {
    return (state_bits & payload_ready_bit_) != 0U;
  }

  [[nodiscard]] static auto try_arrived(const std::uint8_t state_bits) noexcept -> bool {
    return (state_bits & try_arrived_bit_) != 0U;
  }

  [[nodiscard]] static auto claimed(const std::uint8_t state_bits) noexcept -> bool {
    return (state_bits & claimed_bit_) != 0U;
  }

private:
  std::atomic<std::uint8_t> state_bits_{0U};
};

template <typename error_t>
[[nodiscard]] auto to_exception_ptr(error_t &&error) noexcept -> std::exception_ptr {
  if constexpr (std::same_as<std::remove_cvref_t<error_t>, std::exception_ptr>) {
    return std::forward<error_t>(error);
  } else {
    return std::make_exception_ptr(std::forward<error_t>(error));
  }
}

template <typename status_t> struct push_await_error {
  [[nodiscard]] auto operator()(const status_t status) const -> bool {
    if (status == status_t::closed) {
      return false;
    }
    throw status;
  }

  [[nodiscard]] auto operator()(std::exception_ptr error) const -> bool {
    std::rethrow_exception(error);
  }
};

template <typename value_t> struct to_optional_value {
  template <typename value_u>
    requires std::constructible_from<value_t, value_u &&>
  [[nodiscard]] auto operator()(value_u &&value) const
      noexcept(std::is_nothrow_constructible_v<value_t, value_u &&>) -> std::optional<value_t> {
    return std::optional<value_t>{std::forward<value_u>(value)};
  }
};

template <typename value_t, typename status_t> struct pop_await_error {
  [[nodiscard]] auto operator()(const status_t status) const -> std::optional<value_t> {
    if (status == status_t::closed) {
      return std::nullopt;
    }
    throw status;
  }

  [[nodiscard]] auto operator()(std::exception_ptr error) const -> std::optional<value_t> {
    std::rethrow_exception(error);
  }
};

} // namespace detail

template <typename value_t, typename allocator_t = std::allocator<value_t>> class bounded_queue {
private:
  using status_type = bounded_queue_status;
  using critical_section = detail::bounded_queue_critical_section;
  using push_waiter_base_t = detail::push_waiter_base<value_t>;
  using pop_waiter_base_t = detail::pop_waiter_base<value_t>;
  using wait_state_t = detail::wait_state<push_waiter_base_t, pop_waiter_base_t>;
  using storage_t = detail::ring_storage<value_t, allocator_t>;
  static constexpr bool try_pop_is_nothrow = std::is_nothrow_move_constructible_v<value_t> &&
                                             std::is_nothrow_copy_constructible_v<value_t>;

public:
  using value_type = value_t;
  using allocator_type = allocator_t;
  using try_pop_result = result<value_type, status_type>;

  class push_sender;
  class pop_sender;

  explicit bounded_queue(const std::size_t capacity,
                         const allocator_type &allocator = allocator_type{})
      : buffer_(capacity, allocator) {}

  bounded_queue(const bounded_queue &) = delete;
  auto operator=(const bounded_queue &) -> bounded_queue & = delete;
  bounded_queue(bounded_queue &&) = delete;
  auto operator=(bounded_queue &&) -> bounded_queue & = delete;

  ~bounded_queue() noexcept { close(); }

  auto push(const value_type &value) -> bool
    requires std::copy_constructible<value_type>
  {
    return push_blocking_impl(value);
  }

  auto push(value_type &&value) -> bool
    requires std::move_constructible<value_type>
  {
    return push_blocking_impl(std::move(value));
  }

  template <typename... args_t>
    requires std::constructible_from<value_type, args_t &&...>
  auto emplace(args_t &&...args) -> bool {
    value_type value(std::forward<args_t>(args)...);
    return push_blocking_impl(std::move(value));
  }

  [[nodiscard]] auto pop() -> std::optional<value_type> { return pop_blocking_impl(); }

  [[nodiscard]] auto
  try_push(const value_type &value) noexcept(std::is_nothrow_copy_constructible_v<value_type>)
      -> status_type
    requires std::copy_constructible<value_type>
  {
    return try_push_impl(value);
  }

  [[nodiscard]] auto
  try_push(value_type &&value) noexcept(std::is_nothrow_move_constructible_v<value_type>)
      -> status_type
    requires std::move_constructible<value_type>
  {
    return try_push_impl(std::move(value));
  }

  template <typename... args_t>
    requires std::constructible_from<value_type, args_t &&...>
  [[nodiscard]] auto
  try_emplace(args_t &&...args) noexcept(std::is_nothrow_constructible_v<value_type, args_t &&...>)
      -> status_type {
    return try_emplace_impl(std::forward<args_t>(args)...);
  }

  [[nodiscard]] auto try_pop() noexcept(try_pop_is_nothrow) -> try_pop_result {
    return try_pop_impl();
  }

  [[nodiscard]] auto
  async_push(const value_type &value) noexcept(std::is_nothrow_copy_constructible_v<value_type>)
      -> push_sender
    requires std::copy_constructible<value_type>
  {
    return push_sender{this, value};
  }

  [[nodiscard]] auto
  async_push(value_type &&value) noexcept(std::is_nothrow_move_constructible_v<value_type>)
      -> push_sender
    requires std::move_constructible<value_type>
  {
    return push_sender{this, std::move(value)};
  }

  template <typename... args_t>
    requires std::constructible_from<value_type, args_t &&...> &&
             std::move_constructible<value_type>
  [[nodiscard]] auto async_emplace(args_t &&...args) noexcept(
      std::is_nothrow_constructible_v<value_type, args_t &&...> &&
      std::is_nothrow_move_constructible_v<value_type>) -> push_sender {
    return push_sender{std::in_place, this, std::forward<args_t>(args)...};
  }

  [[nodiscard]] auto async_pop() noexcept -> pop_sender { return pop_sender{this}; }

  auto close() noexcept -> void {
    std::optional<typename wait_state_t::detached_waiters> detached{};

    {
      std::unique_lock<critical_section> lock(lock_);
      detached = wait_state_.close_and_detach();
      if (!detached.has_value()) {
        return;
      }

      auto *push_waiter = detached->push_head;
      while (push_waiter != nullptr) {
        auto *current = push_waiter;
        push_waiter = push_waiter->next;
        store_status(*current, status_type::closed);
      }

      auto *pop_waiter = detached->pop_head;
      while (pop_waiter != nullptr) {
        auto *current = pop_waiter;
        pop_waiter = pop_waiter->next;
        if (!buffer_.empty()) {
          try {
            buffer_.consume_front(
                [&](value_type &&value) { store_value(*current, std::move(value)); });
          } catch (...) {
            store_exception(*current, std::current_exception());
          }
        } else {
          store_status(*current, status_type::closed);
        }
      }
    }

    auto *push_waiter = detached->push_head;
    while (push_waiter != nullptr) {
      auto *current = push_waiter;
      push_waiter = push_waiter->next;
      complete_waiter(current);
    }

    auto *pop_waiter = detached->pop_head;
    while (pop_waiter != nullptr) {
      auto *current = pop_waiter;
      pop_waiter = pop_waiter->next;
      complete_waiter(current);
    }
  }

  [[nodiscard]] auto is_closed() const noexcept -> bool {
    std::unique_lock<critical_section> lock(lock_);
    return wait_state_.is_closed();
  }

  [[nodiscard]] auto capacity() const noexcept -> std::size_t { return buffer_.capacity(); }

  [[nodiscard]] auto get_allocator() const noexcept -> allocator_type {
    return buffer_.get_allocator();
  }

  [[nodiscard]] auto size_hint() const noexcept -> std::size_t {
    std::unique_lock<critical_section> lock(lock_);
    return buffer_.size();
  }

private:
  struct sync_push_waiter final : push_waiter_base_t {
    std::atomic_flag ready = ATOMIC_FLAG_INIT;

    explicit sync_push_waiter(value_type &value) noexcept {
      this->set_move_source(value);
      static constexpr typename push_waiter_base_t::ops_type ops{
          [](push_waiter_base_t *base) noexcept {
            auto &self = *static_cast<sync_push_waiter *>(base);
            self.ready.test_and_set(std::memory_order_release);
            self.ready.notify_one();
          },
          nullptr};
      this->ops = &ops;
    }

    explicit sync_push_waiter(const value_type &value) noexcept {
      this->set_copy_source(value);
      static constexpr typename push_waiter_base_t::ops_type ops{
          [](push_waiter_base_t *base) noexcept {
            auto &self = *static_cast<sync_push_waiter *>(base);
            self.ready.test_and_set(std::memory_order_release);
            self.ready.notify_one();
          },
          nullptr};
      this->ops = &ops;
    }

    auto wait() -> status_type {
      ready.wait(false, std::memory_order_acquire);
      if (this->is_error()) {
        std::rethrow_exception(this->error());
      }
      if (this->is_success()) {
        return status_type::success;
      }
      return status_type::closed;
    }
  };

  struct sync_pop_waiter final : pop_waiter_base_t {
    std::atomic_flag ready = ATOMIC_FLAG_INIT;

    sync_pop_waiter() noexcept {
      static constexpr typename pop_waiter_base_t::ops_type ops{
          [](pop_waiter_base_t *base) noexcept {
            auto &self = *static_cast<sync_pop_waiter *>(base);
            self.ready.test_and_set(std::memory_order_release);
            self.ready.notify_one();
          },
          nullptr};
      this->ops = &ops;
    }

    auto wait() -> std::optional<value_type> {
      ready.wait(false, std::memory_order_acquire);
      if (this->is_error()) {
        std::rethrow_exception(this->error());
      }
      if (this->has_value()) {
        return std::move(this->value());
      }
      return std::nullopt;
    }
  };

  template <typename receiver_t> struct push_operation final : push_waiter_base_t {
    using stop_token_t = stdexec::stop_token_of_t<stdexec::env_of_t<receiver_t>>;
    using scheduler_t = detail::resume_scheduler_t<stdexec::env_of_t<receiver_t>>;
    using handoff_sender_t = stdexec::schedule_result_t<scheduler_t>;
    struct stop_callback;
    struct handoff_receiver;
    struct try_handoff_receiver;
    using stop_callback_t = stdexec::stop_callback_for_t<stop_token_t, stop_callback>;
    using handoff_op_t = stdexec::connect_result_t<handoff_sender_t, handoff_receiver>;
    using try_handoff_storage_t = detail::try_handoff_storage<scheduler_t, try_handoff_receiver>;
    using completion_bits_t = detail::completion_bits;
    struct handoff_value_tag {};
    using handoff_completion_t =
        std::variant<handoff_value_tag, std::exception_ptr, detail::stopped_tag>;

    bounded_queue *queue{nullptr};
    receiver_t receiver;
    scheduler_t scheduler;
    value_type value;
    // Lazy handoff_op: only constructed when scheduler handoff is needed.
    // Avoids connect(schedule(scheduler), ...) cost on the fast path.
    alignas(handoff_op_t) std::byte handoff_op_storage_[sizeof(handoff_op_t)];
    bool handoff_op_constructed_{false};
    wh_no_unique_address try_handoff_storage_t try_handoff_{};
    completion_bits_t completion_bits_{};
    std::optional<stop_callback_t> stop_callback_{};
    std::optional<handoff_completion_t> handoff_completion_{};
    std::atomic<bool> handoff_completion_ready_{false};
    std::atomic<bool> handoff_start_returned_{true};

    auto *handoff_op_ptr() noexcept {
      return std::launder(reinterpret_cast<handoff_op_t *>(handoff_op_storage_));
    }

    auto reset_handoff_op() noexcept -> void {
      if (!handoff_op_constructed_) {
        return;
      }
      handoff_op_ptr()->~handoff_op_t();
      handoff_op_constructed_ = false;
    }

    void construct_handoff_op() {
      ::new (static_cast<void *>(handoff_op_storage_))
          handoff_op_t(stdexec::connect(stdexec::schedule(scheduler), handoff_receiver{this}));
      handoff_op_constructed_ = true;
    }

    auto ensure_completion_handoff() -> void {
      if (is_same_scheduler() || handoff_op_constructed_) {
        return;
      }
      construct_handoff_op();
    }

    struct stop_callback {
      push_operation *self{nullptr};

      auto operator()() const noexcept -> void { self->cancel_wait(); }
    };

    struct handoff_receiver {
      using receiver_concept = stdexec::receiver_t;

      push_operation *self{nullptr};

      auto set_value() noexcept -> void {
        self->publish_handoff_completion(handoff_completion_t{handoff_value_tag{}});
      }

      template <typename error_t> auto set_error(error_t &&error) noexcept -> void {
        self->publish_handoff_completion(
            handoff_completion_t{detail::to_exception_ptr(std::forward<error_t>(error))});
      }

      auto set_stopped() noexcept -> void {
        self->publish_handoff_completion(handoff_completion_t{detail::stopped_tag{}});
      }

      [[nodiscard]] auto get_env() const noexcept -> stdexec::env<> { return {}; }
    };

    struct try_handoff_receiver {
      using receiver_concept = stdexec::receiver_t;

      push_operation *self{nullptr};

      auto set_value() noexcept -> void {
        const auto state_bits = self->completion_bits_.mark_try_arrived();
        if (completion_bits_t::ready(state_bits)) {
          self->complete();
        }
      }

      template <typename error_t> auto set_error(error_t &&) noexcept -> void {
        [[maybe_unused]] const auto _ = self->completion_bits_.mark_try_blocked();
      }

      auto set_stopped() noexcept -> void {
        [[maybe_unused]] const auto _ = self->completion_bits_.mark_try_blocked();
      }

      [[nodiscard]] auto get_env() const noexcept -> stdexec::env<> { return {}; }
    };

    template <typename stored_value_t, typename receiver_value_t>
      requires std::constructible_from<value_type, stored_value_t &&> &&
                   std::constructible_from<receiver_t, receiver_value_t &&>
    push_operation(bounded_queue *queue_ptr, stored_value_t &&stored_value,
                   receiver_value_t &&receiver_value)
        : queue(queue_ptr), receiver(std::forward<receiver_value_t>(receiver_value)),
          scheduler(
              detail::select_resume_scheduler<stdexec::set_value_t>(stdexec::get_env(receiver))),
          value(std::forward<stored_value_t>(stored_value)) {
      this->set_move_source(value);
      static constexpr typename push_waiter_base_t::ops_type ops{
          [](push_waiter_base_t *base) noexcept {
            static_cast<push_operation *>(base)->complete_deferred();
          },
          [](push_waiter_base_t *base) noexcept -> bool {
            return static_cast<push_operation *>(base)->try_complete();
          }};
      this->ops = &ops;
    }

    push_operation(const push_operation &) = delete;
    auto operator=(const push_operation &) -> push_operation & = delete;
    push_operation(push_operation &&) = delete;
    auto operator=(push_operation &&) -> push_operation & = delete;

    ~push_operation() { reset_handoff_op(); }

    [[nodiscard]] auto is_same_scheduler() const noexcept -> bool {
      return wh::core::detail::scheduler_handoff::same_scheduler(scheduler);
    }

    auto complete_stopped() noexcept -> void {
      store_stopped(*this);
      complete_deferred();
    }

    // Self completion can still inline on the same scheduler when it does not
    // transfer progress to another waiter.
    auto complete_immediate() noexcept -> void {
      if (is_same_scheduler()) {
        finish();
        return;
      }
      complete_deferred();
    }

    auto complete_deferred() noexcept -> void {
      const auto state_bits = completion_bits_.mark_ready();
      if (completion_bits_t::claimed(state_bits)) {
        if (completion_bits_t::try_arrived(state_bits)) {
          complete();
        }
        return;
      }
      if (is_same_scheduler()) {
        complete();
        return;
      }
      try {
        ensure_completion_handoff();
        wh_invariant(handoff_op_constructed_);
        handoff_start_returned_.store(false, std::memory_order_release);
        stdexec::start(*handoff_op_ptr());
        handoff_start_returned_.store(true, std::memory_order_release);
        if (handoff_completion_ready_.load(std::memory_order_acquire)) {
          drain_handoff_completion();
        }
      } catch (...) {
        handoff_start_returned_.store(true, std::memory_order_release);
        reset_handoff_op();
        store_exception(*this, std::current_exception());
        complete();
      }
    }

    auto prepare_wait(const stop_token_t &stop_token) noexcept -> bool {
      try {
        ensure_completion_handoff();
      } catch (...) {
        store_exception(*this, std::current_exception());
        complete_immediate();
        return false;
      }

      if constexpr (!stdexec::unstoppable_token<stop_token_t>) {
        if (!stop_callback_.has_value()) {
          try {
            stop_callback_.emplace(stop_token, stop_callback{this});
          } catch (...) {
            store_exception(*this, std::current_exception());
            complete_immediate();
            return false;
          }
        }
        if (stop_token.stop_requested()) {
          store_stopped(*this);
          complete_immediate();
          return false;
        }
      }
      return true;
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
      reset_handoff_op();

      if (std::holds_alternative<std::exception_ptr>(completion)) {
        store_exception(*this, std::move(std::get<std::exception_ptr>(completion)));
      } else if (std::holds_alternative<detail::stopped_tag>(completion)) {
        store_stopped(*this);
      }
      complete();
    }

    // Inline delivery — no atomics, no handoff. Used on fast path when
    // same_scheduler is true and no cancel race is possible.
    auto finish() noexcept -> void {
      if (this->is_stopped()) {
        stdexec::set_stopped(std::move(receiver));
        return;
      }
      if (this->is_error()) {
        stdexec::set_error(std::move(receiver), this->error());
        return;
      }
      if (this->is_success()) {
        stdexec::set_value(std::move(receiver));
        return;
      }
      stdexec::set_error(std::move(receiver), status_type::closed);
    }

    auto complete() noexcept -> void {
      if (!completion_bits_.start_delivery()) {
        return;
      }
      finish();
    }

    auto ensure_try_handoff() noexcept -> bool {
      if constexpr (!wh::core::try_scheduler<scheduler_t>) {
        return false;
      } else {
        return try_handoff_.ensure_handoff([this]() {
          return stdexec::connect(
              wh::core::detail::scheduler_handoff::make_try_schedule_handoff_sender(scheduler),
              try_handoff_receiver{this});
        });
      }
    }

    auto try_complete() noexcept -> bool {
      if (is_same_scheduler()) {
        if (!completion_bits_.claim_try_handoff()) {
          return true;
        }
        const auto state_bits = completion_bits_.mark_try_arrived();
        if (completion_bits_t::ready(state_bits)) {
          complete();
        }
        return true;
      }
      if constexpr (!wh::core::try_scheduler<scheduler_t>) {
        return true;
      } else {
        if (!ensure_try_handoff()) {
          return false;
        }
        if (!completion_bits_.claim_try_handoff()) {
          return true;
        }
        stdexec::start(try_handoff_.state());
        if (completion_bits_.try_blocked()) {
          completion_bits_.release_claim();
          completion_bits_.reset_try_handoff();
          try_handoff_.reset();
          return false;
        }
        return true;
      }
    }

    auto cancel_wait() noexcept -> void {
      if (queue == nullptr || completion_bits_.has_claimed()) {
        return;
      }

      std::unique_lock<critical_section> lock(queue->lock_);
      if (queue->wait_state_.remove_push(this)) {
        lock.unlock();
        complete_stopped();
      }
    }

    using operation_state_concept = stdexec::operation_state_t;

    auto start() & noexcept -> void {
      auto &op = *this;
      auto stop_token = stdexec::get_stop_token(stdexec::get_env(op.receiver));
      if (stop_token.stop_requested()) {
        store_stopped(op);
        op.complete_immediate();
        return;
      }

      bool wait_prepared = false;
      for (;;) {
        pop_waiter_base_t *ready_pop = nullptr;
        bool should_prepare_wait = false;
        bool enqueued = false;

        {
          std::unique_lock<critical_section> lock(op.queue->lock_);
          if (op.queue->wait_state_.is_closed()) {
            store_status(op, status_type::closed);
          } else if (auto *waiter = op.queue->wait_state_.take_pop()) {
            ready_pop = waiter;
            store_status(op, status_type::success);
          } else if (!op.queue->buffer_.full()) {
            try {
              op.queue->buffer_.push_back(std::move(op.value));
              store_status(op, status_type::success);
            } catch (...) {
              store_exception(op, std::current_exception());
            }
          } else if (!wait_prepared) {
            should_prepare_wait = true;
          } else {
            op.queue->wait_state_.enqueue_push(&op);
            enqueued = true;
          }
        }

        if (ready_pop != nullptr) {
          try {
            store_value(*ready_pop, std::move(op.value));
          } catch (...) {
            store_exception(*ready_pop, std::current_exception());
          }
          complete_waiter(ready_pop);
          op.complete_immediate();
          return;
        }

        if (enqueued) {
          if constexpr (!stdexec::unstoppable_token<stop_token_t>) {
            if (stop_token.stop_requested()) {
              op.cancel_wait();
            }
          }
          return;
        }

        if (should_prepare_wait) {
          if (!op.prepare_wait(stop_token)) {
            return;
          }
          wait_prepared = true;
          continue;
        }

        op.complete_immediate();
        return;
      }
    }
  };

  template <typename receiver_t> struct pop_operation final : pop_waiter_base_t {
    using stop_token_t = stdexec::stop_token_of_t<stdexec::env_of_t<receiver_t>>;
    using scheduler_t = detail::resume_scheduler_t<stdexec::env_of_t<receiver_t>>;
    using handoff_sender_t = stdexec::schedule_result_t<scheduler_t>;
    struct stop_callback;
    struct handoff_receiver;
    struct try_handoff_receiver;
    using stop_callback_t = stdexec::stop_callback_for_t<stop_token_t, stop_callback>;
    using handoff_op_t = stdexec::connect_result_t<handoff_sender_t, handoff_receiver>;
    using try_handoff_storage_t = detail::try_handoff_storage<scheduler_t, try_handoff_receiver>;
    using completion_bits_t = detail::completion_bits;
    struct handoff_value_tag {};
    using handoff_completion_t =
        std::variant<handoff_value_tag, std::exception_ptr, detail::stopped_tag>;

    bounded_queue *queue{nullptr};
    receiver_t receiver;
    scheduler_t scheduler;
    // Lazy handoff_op: only constructed when scheduler handoff is needed.
    alignas(handoff_op_t) std::byte handoff_op_storage_[sizeof(handoff_op_t)];
    bool handoff_op_constructed_{false};
    wh_no_unique_address try_handoff_storage_t try_handoff_{};
    completion_bits_t completion_bits_{};
    std::optional<stop_callback_t> stop_callback_{};
    std::optional<handoff_completion_t> handoff_completion_{};
    std::atomic<bool> handoff_completion_ready_{false};
    std::atomic<bool> handoff_start_returned_{true};

    auto *handoff_op_ptr() noexcept {
      return std::launder(reinterpret_cast<handoff_op_t *>(handoff_op_storage_));
    }

    auto reset_handoff_op() noexcept -> void {
      if (!handoff_op_constructed_) {
        return;
      }
      handoff_op_ptr()->~handoff_op_t();
      handoff_op_constructed_ = false;
    }

    void construct_handoff_op() {
      ::new (static_cast<void *>(handoff_op_storage_))
          handoff_op_t(stdexec::connect(stdexec::schedule(scheduler), handoff_receiver{this}));
      handoff_op_constructed_ = true;
    }

    auto ensure_completion_handoff() -> void {
      if (is_same_scheduler() || handoff_op_constructed_) {
        return;
      }
      construct_handoff_op();
    }

    struct stop_callback {
      pop_operation *self{nullptr};

      auto operator()() const noexcept -> void { self->cancel_wait(); }
    };

    struct handoff_receiver {
      using receiver_concept = stdexec::receiver_t;

      pop_operation *self{nullptr};

      auto set_value() noexcept -> void {
        self->publish_handoff_completion(handoff_completion_t{handoff_value_tag{}});
      }

      template <typename error_t> auto set_error(error_t &&error) noexcept -> void {
        self->publish_handoff_completion(
            handoff_completion_t{detail::to_exception_ptr(std::forward<error_t>(error))});
      }

      auto set_stopped() noexcept -> void {
        self->publish_handoff_completion(handoff_completion_t{detail::stopped_tag{}});
      }

      [[nodiscard]] auto get_env() const noexcept -> stdexec::env<> { return {}; }
    };

    struct try_handoff_receiver {
      using receiver_concept = stdexec::receiver_t;

      pop_operation *self{nullptr};

      auto set_value() noexcept -> void {
        const auto state_bits = self->completion_bits_.mark_try_arrived();
        if (completion_bits_t::ready(state_bits)) {
          self->complete();
        }
      }

      template <typename error_t> auto set_error(error_t &&) noexcept -> void {
        [[maybe_unused]] const auto _ = self->completion_bits_.mark_try_blocked();
      }

      auto set_stopped() noexcept -> void {
        [[maybe_unused]] const auto _ = self->completion_bits_.mark_try_blocked();
      }

      [[nodiscard]] auto get_env() const noexcept -> stdexec::env<> { return {}; }
    };

    template <typename receiver_value_t>
      requires std::constructible_from<receiver_t, receiver_value_t &&>
    pop_operation(bounded_queue *queue_ptr, receiver_value_t &&receiver_value)
        : queue(queue_ptr), receiver(std::forward<receiver_value_t>(receiver_value)),
          scheduler(
              detail::select_resume_scheduler<stdexec::set_value_t>(stdexec::get_env(receiver))) {
      static constexpr typename pop_waiter_base_t::ops_type ops{
          [](pop_waiter_base_t *base) noexcept {
            static_cast<pop_operation *>(base)->complete_deferred();
          },
          [](pop_waiter_base_t *base) noexcept -> bool {
            return static_cast<pop_operation *>(base)->try_complete();
          }};
      this->ops = &ops;
    }

    pop_operation(const pop_operation &) = delete;
    auto operator=(const pop_operation &) -> pop_operation & = delete;
    pop_operation(pop_operation &&) = delete;
    auto operator=(pop_operation &&) -> pop_operation & = delete;

    ~pop_operation() { reset_handoff_op(); }

    [[nodiscard]] auto is_same_scheduler() const noexcept -> bool {
      return wh::core::detail::scheduler_handoff::same_scheduler(scheduler);
    }

    auto complete_stopped() noexcept -> void {
      store_stopped(*this);
      complete_deferred();
    }

    // Self completion can still inline on the same scheduler when it does not
    // transfer progress to another waiter.
    auto complete_immediate() noexcept -> void {
      if (is_same_scheduler()) {
        finish();
        return;
      }
      complete_deferred();
    }

    auto complete_deferred() noexcept -> void {
      const auto state_bits = completion_bits_.mark_ready();
      if (completion_bits_t::claimed(state_bits)) {
        if (completion_bits_t::try_arrived(state_bits)) {
          complete();
        }
        return;
      }
      if (is_same_scheduler()) {
        complete();
        return;
      }
      try {
        ensure_completion_handoff();
        wh_invariant(handoff_op_constructed_);
        handoff_start_returned_.store(false, std::memory_order_release);
        stdexec::start(*handoff_op_ptr());
        handoff_start_returned_.store(true, std::memory_order_release);
        if (handoff_completion_ready_.load(std::memory_order_acquire)) {
          drain_handoff_completion();
        }
      } catch (...) {
        handoff_start_returned_.store(true, std::memory_order_release);
        reset_handoff_op();
        store_exception(*this, std::current_exception());
        complete();
      }
    }

    auto prepare_wait(const stop_token_t &stop_token) noexcept -> bool {
      try {
        ensure_completion_handoff();
      } catch (...) {
        store_exception(*this, std::current_exception());
        complete_immediate();
        return false;
      }

      if constexpr (!stdexec::unstoppable_token<stop_token_t>) {
        if (!stop_callback_.has_value()) {
          try {
            stop_callback_.emplace(stop_token, stop_callback{this});
          } catch (...) {
            store_exception(*this, std::current_exception());
            complete_immediate();
            return false;
          }
        }
        if (stop_token.stop_requested()) {
          store_stopped(*this);
          complete_immediate();
          return false;
        }
      }
      return true;
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
      reset_handoff_op();

      if (std::holds_alternative<std::exception_ptr>(completion)) {
        store_exception(*this, std::move(std::get<std::exception_ptr>(completion)));
      } else if (std::holds_alternative<detail::stopped_tag>(completion)) {
        store_stopped(*this);
      }
      complete();
    }

    auto finish() noexcept -> void {
      if (this->is_stopped()) {
        stdexec::set_stopped(std::move(receiver));
        return;
      }
      if (this->is_error()) {
        stdexec::set_error(std::move(receiver), this->error());
        return;
      }
      if (this->has_value()) {
        stdexec::set_value(std::move(receiver), std::move(this->value()));
        return;
      }
      stdexec::set_error(std::move(receiver), status_type::closed);
    }

    auto complete() noexcept -> void {
      if (!completion_bits_.start_delivery()) {
        return;
      }
      finish();
    }

    auto ensure_try_handoff() noexcept -> bool {
      if constexpr (!wh::core::try_scheduler<scheduler_t>) {
        return false;
      } else {
        return try_handoff_.ensure_handoff([this]() {
          return stdexec::connect(
              wh::core::detail::scheduler_handoff::make_try_schedule_handoff_sender(scheduler),
              try_handoff_receiver{this});
        });
      }
    }

    auto try_complete() noexcept -> bool {
      if (is_same_scheduler()) {
        if (!completion_bits_.claim_try_handoff()) {
          return true;
        }
        const auto state_bits = completion_bits_.mark_try_arrived();
        if (completion_bits_t::ready(state_bits)) {
          complete();
        }
        return true;
      }
      if constexpr (!wh::core::try_scheduler<scheduler_t>) {
        return true;
      } else {
        if (!ensure_try_handoff()) {
          return false;
        }
        if (!completion_bits_.claim_try_handoff()) {
          return true;
        }
        stdexec::start(try_handoff_.state());
        if (completion_bits_.try_blocked()) {
          completion_bits_.release_claim();
          completion_bits_.reset_try_handoff();
          try_handoff_.reset();
          return false;
        }
        return true;
      }
    }

    auto cancel_wait() noexcept -> void {
      if (queue == nullptr || completion_bits_.has_claimed()) {
        return;
      }

      std::unique_lock<critical_section> lock(queue->lock_);
      if (queue->wait_state_.remove_pop(this)) {
        lock.unlock();
        complete_stopped();
      }
    }

    using operation_state_concept = stdexec::operation_state_t;

    auto start() & noexcept -> void {
      auto &op = *this;
      auto stop_token = stdexec::get_stop_token(stdexec::get_env(op.receiver));
      if (stop_token.stop_requested()) {
        store_stopped(op);
        op.complete_immediate();
        return;
      }

      bool wait_prepared = false;
      for (;;) {
        push_waiter_base_t *ready_push = nullptr;
        bool ready_zero_capacity_push = false;
        bool should_prepare_wait = false;
        bool enqueued = false;

        {
          std::unique_lock<critical_section> lock(op.queue->lock_);
          if (!op.queue->buffer_.empty()) {
            try {
              op.queue->buffer_.consume_front(
                  [&](value_type &&value) { store_value(op, std::move(value)); });
            } catch (...) {
              store_exception(op, std::current_exception());
            }
            if (auto *waiter = op.queue->wait_state_.take_push()) {
              try {
                op.queue->push_waiter_into_ring(*waiter);
                store_status(*waiter, status_type::success);
              } catch (...) {
                store_exception(*waiter, std::current_exception());
              }
              ready_push = waiter;
            }
          } else if (auto *waiter = op.queue->wait_state_.take_push();
                     waiter != nullptr && op.queue->buffer_.capacity() == 0U) {
            ready_push = waiter;
            ready_zero_capacity_push = true;
          } else if (op.queue->wait_state_.is_closed()) {
            store_status(op, status_type::closed);
          } else if (!wait_prepared) {
            should_prepare_wait = true;
          } else {
            op.queue->wait_state_.enqueue_pop(&op);
            enqueued = true;
          }
        }

        if (ready_zero_capacity_push) {
          try {
            op.queue->visit_push_source(*ready_push, [&](auto &&value) {
              store_value(op, std::forward<decltype(value)>(value));
            });
            store_status(*ready_push, status_type::success);
          } catch (...) {
            auto error = std::current_exception();
            store_exception(op, error);
            store_exception(*ready_push, std::move(error));
          }
        }

        if (ready_push != nullptr) {
          complete_waiter(ready_push);
          op.complete_immediate();
          return;
        }

        if (enqueued) {
          if constexpr (!stdexec::unstoppable_token<stop_token_t>) {
            if (stop_token.stop_requested()) {
              op.cancel_wait();
            }
          }
          return;
        }

        if (should_prepare_wait) {
          if (!op.prepare_wait(stop_token)) {
            return;
          }
          wait_prepared = true;
          continue;
        }

        op.complete_immediate();
        return;
      }
    }
  };

public:
  class push_sender {
  public:
    using sender_concept = stdexec::sender_t;
    using is_sender = void;
    using bounded_queue_push_sender_tag = void;
    using bounded_queue_value_type = value_type;
    using bounded_queue_allocator_type = allocator_type;
    using completion_signatures =
        stdexec::completion_signatures<stdexec::set_value_t(), stdexec::set_error_t(status_type),
                                       stdexec::set_error_t(std::exception_ptr),
                                       stdexec::set_stopped_t()>;

    push_sender(bounded_queue *queue_ptr, const value_type &stored_value) noexcept(
        std::is_nothrow_copy_constructible_v<value_type>)
      requires std::copy_constructible<value_type>
        : queue_(queue_ptr), value_(stored_value) {}

    push_sender(bounded_queue *queue_ptr, value_type &&stored_value) noexcept(
        std::is_nothrow_move_constructible_v<value_type>)
      requires std::move_constructible<value_type>
        : queue_(queue_ptr), value_(std::move(stored_value)) {}

    template <typename... args_t>
      requires std::constructible_from<value_type, args_t &&...>
    push_sender(std::in_place_t, bounded_queue *queue_ptr, args_t &&...args) noexcept(
        std::is_nothrow_constructible_v<value_type, args_t &&...>)
        : queue_(queue_ptr), value_(std::forward<args_t>(args)...) {}

    template <stdexec::receiver_of<completion_signatures> receiver_t>
      requires detail::receiver_with_resume_scheduler<receiver_t>
    [[nodiscard]] auto
    connect(receiver_t receiver) && -> push_operation<std::remove_cvref_t<receiver_t>> {
      return push_operation<std::remove_cvref_t<receiver_t>>{queue_, std::move(value_),
                                                             std::move(receiver)};
    }

    template <stdexec::receiver_of<completion_signatures> receiver_t>
      requires detail::receiver_with_resume_scheduler<receiver_t>
    [[nodiscard]] auto
    connect(receiver_t receiver) const & -> push_operation<std::remove_cvref_t<receiver_t>>
      requires std::copy_constructible<value_type>
    {
      return push_operation<std::remove_cvref_t<receiver_t>>{queue_, value_, std::move(receiver)};
    }

    template <typename promise_t>
      requires detail::promise_with_resume_scheduler<promise_t>
    [[nodiscard]] auto as_awaitable(promise_t &promise) && -> decltype(auto) {
      return stdexec::as_awaitable(make_await_sender(std::move(*this)), promise);
    }

    template <typename promise_t>
      requires detail::promise_with_resume_scheduler<promise_t>
    [[nodiscard]] auto as_awaitable(promise_t &promise) const & -> decltype(auto)
      requires std::copy_constructible<value_type>
    {
      return stdexec::as_awaitable(make_await_sender(*this), promise);
    }

    [[nodiscard]] auto get_env() const noexcept -> detail::async_completion_env { return {}; }

  private:
    template <typename self_t> [[nodiscard]] static auto make_await_sender(self_t &&self) {
      return stdexec::upon_error(
          stdexec::then(static_cast<self_t &&>(self), []() noexcept -> bool { return true; }),
          detail::push_await_error<status_type>{});
    }

    bounded_queue *queue_{nullptr};
    value_type value_;
  };

  class pop_sender {
  public:
    using sender_concept = stdexec::sender_t;
    using is_sender = void;
    using bounded_queue_pop_sender_tag = void;
    using bounded_queue_value_type = value_type;
    using bounded_queue_allocator_type = allocator_type;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(value_type), stdexec::set_error_t(status_type),
        stdexec::set_error_t(std::exception_ptr), stdexec::set_stopped_t()>;

    explicit pop_sender(bounded_queue *queue_ptr) noexcept : queue_(queue_ptr) {}

    template <stdexec::receiver_of<completion_signatures> receiver_t>
      requires detail::receiver_with_resume_scheduler<receiver_t>
    [[nodiscard]] auto
    connect(receiver_t receiver) && -> pop_operation<std::remove_cvref_t<receiver_t>> {
      return pop_operation<std::remove_cvref_t<receiver_t>>{queue_, std::move(receiver)};
    }

    template <stdexec::receiver_of<completion_signatures> receiver_t>
      requires detail::receiver_with_resume_scheduler<receiver_t>
    [[nodiscard]] auto
    connect(receiver_t receiver) const & -> pop_operation<std::remove_cvref_t<receiver_t>> {
      return pop_operation<std::remove_cvref_t<receiver_t>>{queue_, std::move(receiver)};
    }

    template <typename promise_t>
      requires detail::promise_with_resume_scheduler<promise_t>
    [[nodiscard]] auto as_awaitable(promise_t &promise) && -> decltype(auto) {
      return stdexec::as_awaitable(make_await_sender(std::move(*this)), promise);
    }

    template <typename promise_t>
      requires detail::promise_with_resume_scheduler<promise_t>
    [[nodiscard]] auto as_awaitable(promise_t &promise) const & -> decltype(auto) {
      return stdexec::as_awaitable(make_await_sender(*this), promise);
    }

    [[nodiscard]] auto get_env() const noexcept -> detail::async_completion_env { return {}; }

  private:
    template <typename self_t> [[nodiscard]] static auto make_await_sender(self_t &&self) {
      return stdexec::upon_error(
          stdexec::then(static_cast<self_t &&>(self), detail::to_optional_value<value_type>{}),
          detail::pop_await_error<value_type, status_type>{});
    }

    bounded_queue *queue_{nullptr};
  };

private:
  template <typename value_u>
  static auto store_value(pop_waiter_base_t &waiter, value_u &&value) -> void {
    waiter.emplace_value(std::forward<value_u>(value));
  }

  template <typename... args_t>
  static auto emplace_value(pop_waiter_base_t &waiter, args_t &&...args) -> void {
    waiter.emplace_value(std::forward<args_t>(args)...);
  }

  static auto store_status(push_waiter_base_t &waiter, const status_type status) -> void {
    if (status == status_type::success) {
      waiter.store_success();
      return;
    }
    waiter.store_closed();
  }

  static auto store_status(pop_waiter_base_t &waiter, const status_type) -> void {
    waiter.store_closed();
  }

  static auto store_exception(push_waiter_base_t &waiter, std::exception_ptr error) noexcept
      -> void {
    waiter.store_error(std::move(error));
  }

  static auto store_exception(pop_waiter_base_t &waiter, std::exception_ptr error) noexcept
      -> void {
    waiter.store_error(std::move(error));
  }

  static auto store_stopped(push_waiter_base_t &waiter) noexcept -> void { waiter.store_stopped(); }

  static auto store_stopped(pop_waiter_base_t &waiter) noexcept -> void { waiter.store_stopped(); }

  template <typename waiter_t> static auto complete_waiter(waiter_t *waiter) noexcept -> void {
    waiter->ops->complete(waiter);
  }

  template <typename waiter_t>
  [[nodiscard]] static auto has_try_complete(const waiter_t *waiter) noexcept -> bool {
    return waiter != nullptr && waiter->ops != nullptr && waiter->ops->try_complete != nullptr;
  }

  template <typename waiter_t>
  [[nodiscard]] static auto try_complete_waiter(waiter_t *waiter) noexcept -> bool {
    return waiter != nullptr && waiter->ops != nullptr && waiter->ops->try_complete != nullptr &&
           waiter->ops->try_complete(waiter);
  }

  [[noreturn]] static auto rethrow_or_terminate_try_pop(
      [[maybe_unused]] std::exception_ptr error) noexcept(try_pop_is_nothrow) -> void {
    if constexpr (try_pop_is_nothrow) {
      std::terminate();
    } else {
      std::rethrow_exception(std::move(error));
    }
  }

  template <typename sink_t>
  auto visit_push_source(push_waiter_base_t &waiter, sink_t &&sink) -> void {
    if (waiter.has_move_source()) {
      std::forward<sink_t>(sink)(std::move(waiter.movable_source_value()));
      return;
    }
    if constexpr (std::copy_constructible<value_type>) {
      if (waiter.has_copy_source()) {
        std::forward<sink_t>(sink)(waiter.source_value());
        return;
      }
    }
    unreachable();
  }

  auto push_waiter_into_ring(push_waiter_base_t &waiter) -> void {
    visit_push_source(
        waiter, [&](auto &&value) { buffer_.push_back(std::forward<decltype(value)>(value)); });
  }

  template <typename value_u> auto push_blocking_impl(value_u &&value) -> bool {
    pop_waiter_base_t *ready_pop = nullptr;
    std::exception_ptr operation_error{};

    {
      std::unique_lock<critical_section> lock(lock_);
      if (wait_state_.is_closed()) {
        return false;
      }

      if (auto *waiter = wait_state_.take_pop()) {
        ready_pop = waiter;
      } else if (!buffer_.full()) {
        buffer_.push_back(std::forward<value_u>(value));
        return true;
      } else {
        sync_push_waiter blocked_waiter{value};
        wait_state_.enqueue_push(&blocked_waiter);
        lock.unlock();
        return blocked_waiter.wait() == status_type::success;
      }
    }

    try {
      store_value(*ready_pop, std::forward<value_u>(value));
    } catch (...) {
      operation_error = std::current_exception();
      store_exception(*ready_pop, operation_error);
    }
    complete_waiter(ready_pop);
    if (operation_error != nullptr) {
      std::rethrow_exception(operation_error);
    }
    return true;
  }

  [[nodiscard]] auto pop_blocking_impl() -> std::optional<value_type> {
    push_waiter_base_t *ready_push = nullptr;
    bool ready_zero_capacity_push = false;
    std::optional<value_type> ready_value{};
    std::exception_ptr operation_error{};

    {
      std::unique_lock<critical_section> lock(lock_);
      if (!buffer_.empty()) {
        buffer_.consume_front([&](value_type &&value) { ready_value.emplace(std::move(value)); });
        if (auto *waiter = wait_state_.take_push()) {
          try {
            push_waiter_into_ring(*waiter);
            store_status(*waiter, status_type::success);
          } catch (...) {
            store_exception(*waiter, std::current_exception());
          }
          ready_push = waiter;
        }
      } else if (auto *waiter = wait_state_.take_push();
                 waiter != nullptr && buffer_.capacity() == 0U) {
        ready_push = waiter;
        ready_zero_capacity_push = true;
      } else if (wait_state_.is_closed()) {
        return std::nullopt;
      } else {
        sync_pop_waiter blocked_waiter{};
        wait_state_.enqueue_pop(&blocked_waiter);
        lock.unlock();
        return blocked_waiter.wait();
      }
    }

    if (ready_zero_capacity_push) {
      try {
        visit_push_source(*ready_push, [&](auto &&value) {
          ready_value.emplace(std::forward<decltype(value)>(value));
        });
        store_status(*ready_push, status_type::success);
      } catch (...) {
        operation_error = std::current_exception();
        store_exception(*ready_push, operation_error);
      }
    }

    if (ready_push != nullptr) {
      complete_waiter(ready_push);
    }
    if (operation_error != nullptr) {
      std::rethrow_exception(operation_error);
    }
    return ready_value;
  }

  template <typename value_u>
  [[nodiscard]] auto
  try_push_impl(value_u &&value) noexcept(std::is_nothrow_constructible_v<value_type, value_u &&>)
      -> status_type {
    pop_waiter_base_t *ready_pop = nullptr;
    std::exception_ptr operation_error{};
    bool buffered = false;

    {
      std::unique_lock<critical_section> lock(lock_, std::try_to_lock);
      if (!lock.owns_lock()) {
        return status_type::busy;
      }
      if (wait_state_.is_closed()) {
        return status_type::closed;
      }

      if (auto *front = wait_state_.front_pop()) {
        if (has_try_complete(front) && !try_complete_waiter(front)) {
          return status_type::busy_async;
        }
        ready_pop = wait_state_.take_pop();
      } else if (buffer_.full()) {
        return status_type::full;
      } else {
        buffer_.push_back(std::forward<value_u>(value));
        buffered = true;
      }
    }

    if (buffered) {
      return status_type::success;
    }

    try {
      store_value(*ready_pop, std::forward<value_u>(value));
    } catch (...) {
      operation_error = std::current_exception();
      store_exception(*ready_pop, operation_error);
    }
    complete_waiter(ready_pop);
    if (operation_error != nullptr) {
      std::rethrow_exception(operation_error);
    }
    return status_type::success;
  }

  template <typename... args_t>
  [[nodiscard]] auto try_emplace_impl(args_t &&...args) noexcept(
      std::is_nothrow_constructible_v<value_type, args_t &&...>) -> status_type {
    pop_waiter_base_t *ready_pop = nullptr;
    std::exception_ptr operation_error{};
    bool buffered = false;

    {
      std::unique_lock<critical_section> lock(lock_, std::try_to_lock);
      if (!lock.owns_lock()) {
        return status_type::busy;
      }
      if (wait_state_.is_closed()) {
        return status_type::closed;
      }

      if (auto *front = wait_state_.front_pop()) {
        if (has_try_complete(front) && !try_complete_waiter(front)) {
          return status_type::busy_async;
        }
        ready_pop = wait_state_.take_pop();
      } else if (buffer_.full()) {
        return status_type::full;
      } else {
        buffer_.emplace_back(std::forward<args_t>(args)...);
        buffered = true;
      }
    }

    if (buffered) {
      return status_type::success;
    }

    try {
      emplace_value(*ready_pop, std::forward<args_t>(args)...);
    } catch (...) {
      operation_error = std::current_exception();
      store_exception(*ready_pop, operation_error);
    }
    complete_waiter(ready_pop);
    if (operation_error != nullptr) {
      std::rethrow_exception(operation_error);
    }
    return status_type::success;
  }

  [[nodiscard]] auto try_pop_impl() noexcept(try_pop_is_nothrow) -> try_pop_result {
    push_waiter_base_t *ready_push = nullptr;
    push_waiter_base_t *scheduled_async_push = nullptr;
    push_waiter_base_t *ready_rendezvous = nullptr;

    {
      std::unique_lock<critical_section> lock(lock_, std::try_to_lock);
      if (!lock.owns_lock()) {
        return try_pop_result::failure(status_type::busy);
      }
      if (buffer_.empty()) {
        auto *front_push = wait_state_.front_push();
        if (front_push != nullptr && buffer_.capacity() == 0U) {
          if (has_try_complete(front_push) && !try_complete_waiter(front_push)) {
            return try_pop_result::failure(status_type::busy_async);
          }
          ready_rendezvous = wait_state_.take_push();
          lock.unlock();

          try {
            std::optional<value_type> ready_value{};
            visit_push_source(*ready_rendezvous, [&](auto &&value) {
              ready_value.emplace(std::forward<decltype(value)>(value));
            });
            store_status(*ready_rendezvous, status_type::success);
            complete_waiter(ready_rendezvous);
            return std::move(*ready_value);
          } catch (...) {
            auto error = std::current_exception();
            store_exception(*ready_rendezvous, error);
            complete_waiter(ready_rendezvous);
            rethrow_or_terminate_try_pop(std::move(error));
          }
        }

        if (wait_state_.is_closed()) {
          return try_pop_result::failure(status_type::closed);
        }
        return try_pop_result::failure(status_type::empty);
      }

      if (buffer_.full() && has_try_complete(wait_state_.front_push())) {
        if (!try_complete_waiter(wait_state_.front_push())) {
          return try_pop_result::failure(status_type::busy_async);
        }
        scheduled_async_push = wait_state_.front_push();
      }

      std::optional<value_type> ready_value{};
      try {
        buffer_.consume_front([&](value_type &&value) { ready_value.emplace(std::move(value)); });
      } catch (...) {
        if (scheduled_async_push != nullptr) {
          auto *failed_waiter = wait_state_.take_push();
          if (failed_waiter != nullptr) {
            auto error = std::current_exception();
            store_exception(*failed_waiter, error);
            lock.unlock();
            complete_waiter(failed_waiter);
            rethrow_or_terminate_try_pop(std::move(error));
          }
        }
        rethrow_or_terminate_try_pop(std::current_exception());
      }

      if (auto *waiter = wait_state_.take_push()) {
        try {
          push_waiter_into_ring(*waiter);
          store_status(*waiter, status_type::success);
        } catch (...) {
          store_exception(*waiter, std::current_exception());
        }
        ready_push = waiter;
      }

      lock.unlock();
      if (ready_push != nullptr) {
        complete_waiter(ready_push);
      }
      return std::move(*ready_value);
    }
  }

  mutable critical_section lock_{};
  storage_t buffer_;
  wait_state_t wait_state_{};
};

} // namespace wh::core
