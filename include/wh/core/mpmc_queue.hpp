#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

#include <exec/start_detached.hpp>

#include "wh/async/async_initiate.hpp"
#include "wh/async/completion_token_helper.hpp"
#include "wh/core/compiler.hpp"
#include "wh/core/result.hpp"
#include "wh/scheduler/timer_helper.hpp"
#include "wh/sync/sender_notify.hpp"

namespace wh::core {

struct mpmc_dynamic_options {
  std::size_t max_capacity{0U};
  std::size_t growth_factor{2U};
};

template <typename value_t, bool dynamic_growth = false,
          typename allocator_t = std::allocator<value_t>>
class mpmc_queue;

namespace detail {

using turn_wait_registration = sender_notify::wait_registration;
using turn_waiter = sender_notify::waiter;

template <typename value_t> struct mpmc_queue_common {
  struct alignas(default_cacheline_size) slot {
    std::atomic<std::uint64_t> turn{0U};
    std::aligned_storage_t<sizeof(value_t), alignof(value_t)> storage{};

    [[nodiscard]] auto value_ptr() noexcept -> value_t * {
      return std::launder(reinterpret_cast<value_t *>(&storage));
    }
  };

  static constexpr std::size_t slot_padding =
      ((default_cacheline_size - 1U) / sizeof(slot)) + 1U;

  [[nodiscard]] static auto validate_capacity(const std::size_t capacity)
      -> std::size_t {
    wh_precondition(capacity > 0U);
    return capacity;
  }

  [[nodiscard]] static auto slot_count(const std::size_t capacity) noexcept
      -> std::size_t {
    return capacity + (2U * slot_padding);
  }

  [[nodiscard]] static auto compute_stride(const std::size_t capacity) noexcept
      -> int {
    static constexpr std::array<int, 9U> small_primes{2,  3,  5,  7, 11,
                                                      13, 17, 19, 23};

    int best_stride = 1;
    std::size_t best_separation = 1U;

    for (const int stride : small_primes) {
      const auto stride_value = static_cast<std::size_t>(stride);
      if ((stride_value % capacity) == 0U || (capacity % stride_value) == 0U) {
        continue;
      }

      std::size_t separation = stride_value % capacity;
      separation = std::min(separation, capacity - separation);
      if (separation > best_separation) {
        best_stride = stride;
        best_separation = separation;
      }
    }

    return best_stride;
  }

  [[nodiscard]] static auto enqueue_turn(const std::uint64_t local_ticket,
                                         const std::size_t capacity) noexcept
      -> std::uint64_t {
    if (is_power_of_two(capacity)) {
      const auto shift = static_cast<unsigned>(std::countr_zero(capacity));
      return (local_ticket >> shift) << 1U;
    }
    return (local_ticket / capacity) * 2U;
  }

  [[nodiscard]] static auto dequeue_turn(const std::uint64_t local_ticket,
                                         const std::size_t capacity) noexcept
      -> std::uint64_t {
    return enqueue_turn(local_ticket, capacity) + 1U;
  }
};

template <typename queue_t, typename value_t> class mpmc_queue_async_facade {
public:
  template <scheduler_context_like scheduler_context_t,
            completion_token completion_token_t = use_sender_t>
  [[nodiscard]] auto push(scheduler_context_t context, const value_t &value,
                          completion_token_t token = completion_token_t{})
      -> decltype(auto)
    requires std::is_nothrow_copy_constructible_v<value_t>
  {
    using bare_token_t = std::remove_cvref_t<completion_token_t>;

    if constexpr (std::same_as<bare_token_t, use_sender_t>) {
      return make_push_sender(context, value);
    } else if constexpr (std::same_as<bare_token_t, use_awaitable_t>) {
      return detail::make_awaitable_task<result<void>>(
          make_push_sender(context, value));
    } else if constexpr (callback_token<bare_token_t>) {
      auto callback = std::move(token);
      auto handler = std::move(callback.handler);
      const auto stop_token = callback.stop_token;

      if (stop_token.stop_requested()) {
        std::invoke(handler, result<void>::failure(errc::canceled));
        return;
      }

      auto sender = make_push_sender(context, value);
      exec::start_detached(
          std::move(sender) |
          stdexec::then([handler = std::move(handler),
                         stop_token](result<void> status) mutable {
            if (stop_token.stop_requested() && status.has_value()) {
              status = result<void>::failure(errc::canceled);
            }
            std::invoke(handler, std::move(status));
          }));
      return;
    }
  }

  template <scheduler_context_like scheduler_context_t,
            completion_token completion_token_t = use_sender_t>
  [[nodiscard]] auto push(scheduler_context_t context, value_t &&value,
                          completion_token_t token = completion_token_t{})
      -> decltype(auto)
    requires std::is_nothrow_move_constructible_v<value_t>
  {
    using bare_token_t = std::remove_cvref_t<completion_token_t>;

    if constexpr (std::same_as<bare_token_t, use_sender_t>) {
      return make_push_sender(context, std::move(value));
    } else if constexpr (std::same_as<bare_token_t, use_awaitable_t>) {
      return detail::make_awaitable_task<result<void>>(
          make_push_sender(context, std::move(value)));
    } else if constexpr (callback_token<bare_token_t>) {
      auto callback = std::move(token);
      auto handler = std::move(callback.handler);
      const auto stop_token = callback.stop_token;

      if (stop_token.stop_requested()) {
        std::invoke(handler, result<void>::failure(errc::canceled));
        return;
      }

      auto sender = make_push_sender(context, std::move(value));
      exec::start_detached(
          std::move(sender) |
          stdexec::then([handler = std::move(handler),
                         stop_token](result<void> status) mutable {
            if (stop_token.stop_requested() && status.has_value()) {
              status = result<void>::failure(errc::canceled);
            }
            std::invoke(handler, std::move(status));
          }));
      return;
    }
  }

  template <scheduler_context_like scheduler_context_t,
            completion_token completion_token_t = use_sender_t>
  [[nodiscard]] auto pop(scheduler_context_t context,
                         completion_token_t token = completion_token_t{})
      -> decltype(auto)
    requires std::is_nothrow_move_constructible_v<value_t>
  {
    using bare_token_t = std::remove_cvref_t<completion_token_t>;

    if constexpr (std::same_as<bare_token_t, use_sender_t>) {
      return make_pop_sender(context);
    } else if constexpr (std::same_as<bare_token_t, use_awaitable_t>) {
      return detail::make_awaitable_task<result<value_t>>(
          make_pop_sender(context));
    } else if constexpr (callback_token<bare_token_t>) {
      auto callback = std::move(token);
      auto handler = std::move(callback.handler);
      const auto stop_token = callback.stop_token;

      if (stop_token.stop_requested()) {
        std::invoke(handler, result<value_t>::failure(errc::canceled));
        return;
      }

      auto sender = make_pop_sender(context);
      exec::start_detached(
          std::move(sender) |
          stdexec::then([handler = std::move(handler),
                         stop_token](result<value_t> status) mutable {
            if (stop_token.stop_requested() && status.has_value()) {
              status = result<value_t>::failure(errc::canceled);
            }
            std::invoke(handler, std::move(status));
          }));
      return;
    }
  }

  template <timed_scheduler_in_context scheduler_context_t, typename deadline_t,
            completion_token completion_token_t = use_sender_t>
  [[nodiscard]] auto push_until(scheduler_context_t context,
                                const deadline_t &deadline,
                                const value_t &value,
                                completion_token_t token = completion_token_t{})
      -> decltype(auto)
    requires std::is_nothrow_copy_constructible_v<value_t> &&
             requires(const scheduler_context_t &current_context,
                      const deadline_t &current_deadline) {
               {
                 context_now(current_context) < current_deadline
               } -> std::convertible_to<bool>;
             }
  {
    using bare_token_t = std::remove_cvref_t<completion_token_t>;

    if constexpr (std::same_as<bare_token_t, use_sender_t>) {
      return timeout_at<result<void>>(context, make_push_sender(context, value),
                                      deadline) |
             stdexec::upon_error([](auto &&) noexcept {
               return result<void>::failure(errc::unavailable);
             }) |
             stdexec::upon_stopped([]() noexcept {
               return result<void>::failure(errc::canceled);
             });
    } else if constexpr (std::same_as<bare_token_t, use_awaitable_t>) {
      auto sender = timeout_at<result<void>>(
                        context, make_push_sender(context, value), deadline) |
                    stdexec::upon_error([](auto &&) noexcept {
                      return result<void>::failure(errc::unavailable);
                    }) |
                    stdexec::upon_stopped([]() noexcept {
                      return result<void>::failure(errc::canceled);
                    });
      return detail::make_awaitable_task<result<void>>(std::move(sender));
    } else if constexpr (callback_token<bare_token_t>) {
      auto callback = std::move(token);
      auto handler = std::move(callback.handler);
      const auto stop_token = callback.stop_token;

      if (stop_token.stop_requested()) {
        std::invoke(handler, result<void>::failure(errc::canceled));
        return;
      }

      auto sender = timeout_at<result<void>>(
                        context, make_push_sender(context, value), deadline) |
                    stdexec::upon_error([](auto &&) noexcept {
                      return result<void>::failure(errc::unavailable);
                    }) |
                    stdexec::upon_stopped([]() noexcept {
                      return result<void>::failure(errc::canceled);
                    });
      exec::start_detached(
          std::move(sender) |
          stdexec::then([handler = std::move(handler),
                         stop_token](result<void> status) mutable {
            if (stop_token.stop_requested() && status.has_value()) {
              status = result<void>::failure(errc::canceled);
            }
            std::invoke(handler, std::move(status));
          }));
      return;
    }
  }

  template <timed_scheduler_in_context scheduler_context_t, typename deadline_t,
            completion_token completion_token_t = use_sender_t>
  [[nodiscard]] auto push_until(scheduler_context_t context,
                                const deadline_t &deadline, value_t &&value,
                                completion_token_t token = completion_token_t{})
      -> decltype(auto)
    requires std::is_nothrow_move_constructible_v<value_t> &&
             requires(const scheduler_context_t &current_context,
                      const deadline_t &current_deadline) {
               {
                 context_now(current_context) < current_deadline
               } -> std::convertible_to<bool>;
             }
  {
    using bare_token_t = std::remove_cvref_t<completion_token_t>;

    if constexpr (std::same_as<bare_token_t, use_sender_t>) {
      return timeout_at<result<void>>(
                 context, make_push_sender(context, std::move(value)),
                 deadline) |
             stdexec::upon_error([](auto &&) noexcept {
               return result<void>::failure(errc::unavailable);
             }) |
             stdexec::upon_stopped([]() noexcept {
               return result<void>::failure(errc::canceled);
             });
    } else if constexpr (std::same_as<bare_token_t, use_awaitable_t>) {
      auto sender =
          timeout_at<result<void>>(
              context, make_push_sender(context, std::move(value)), deadline) |
          stdexec::upon_error([](auto &&) noexcept {
            return result<void>::failure(errc::unavailable);
          }) |
          stdexec::upon_stopped(
              []() noexcept { return result<void>::failure(errc::canceled); });
      return detail::make_awaitable_task<result<void>>(std::move(sender));
    } else if constexpr (callback_token<bare_token_t>) {
      auto callback = std::move(token);
      auto handler = std::move(callback.handler);
      const auto stop_token = callback.stop_token;

      if (stop_token.stop_requested()) {
        std::invoke(handler, result<void>::failure(errc::canceled));
        return;
      }

      auto sender =
          timeout_at<result<void>>(
              context, make_push_sender(context, std::move(value)), deadline) |
          stdexec::upon_error([](auto &&) noexcept {
            return result<void>::failure(errc::unavailable);
          }) |
          stdexec::upon_stopped(
              []() noexcept { return result<void>::failure(errc::canceled); });
      exec::start_detached(
          std::move(sender) |
          stdexec::then([handler = std::move(handler),
                         stop_token](result<void> status) mutable {
            if (stop_token.stop_requested() && status.has_value()) {
              status = result<void>::failure(errc::canceled);
            }
            std::invoke(handler, std::move(status));
          }));
      return;
    }
  }

  template <timed_scheduler_in_context scheduler_context_t, typename deadline_t,
            completion_token completion_token_t = use_sender_t>
  [[nodiscard]] auto pop_until(scheduler_context_t context,
                               const deadline_t &deadline,
                               completion_token_t token = completion_token_t{})
      -> decltype(auto)
    requires std::is_nothrow_move_constructible_v<value_t> &&
             requires(const scheduler_context_t &current_context,
                      const deadline_t &current_deadline) {
               {
                 context_now(current_context) < current_deadline
               } -> std::convertible_to<bool>;
             }
  {
    using bare_token_t = std::remove_cvref_t<completion_token_t>;

    if constexpr (std::same_as<bare_token_t, use_sender_t>) {
      return timeout_at<result<value_t>>(context, make_pop_sender(context),
                                         deadline) |
             stdexec::upon_error([](auto &&) noexcept {
               return result<value_t>::failure(errc::unavailable);
             }) |
             stdexec::upon_stopped([]() noexcept {
               return result<value_t>::failure(errc::canceled);
             });
    } else if constexpr (std::same_as<bare_token_t, use_awaitable_t>) {
      auto sender = timeout_at<result<value_t>>(
                        context, make_pop_sender(context), deadline) |
                    stdexec::upon_error([](auto &&) noexcept {
                      return result<value_t>::failure(errc::unavailable);
                    }) |
                    stdexec::upon_stopped([]() noexcept {
                      return result<value_t>::failure(errc::canceled);
                    });
      return detail::make_awaitable_task<result<value_t>>(std::move(sender));
    } else if constexpr (callback_token<bare_token_t>) {
      auto callback = std::move(token);
      auto handler = std::move(callback.handler);
      const auto stop_token = callback.stop_token;

      if (stop_token.stop_requested()) {
        std::invoke(handler, result<value_t>::failure(errc::canceled));
        return;
      }

      auto sender = timeout_at<result<value_t>>(
                        context, make_pop_sender(context), deadline) |
                    stdexec::upon_error([](auto &&) noexcept {
                      return result<value_t>::failure(errc::unavailable);
                    }) |
                    stdexec::upon_stopped([]() noexcept {
                      return result<value_t>::failure(errc::canceled);
                    });
      exec::start_detached(
          std::move(sender) |
          stdexec::then([handler = std::move(handler),
                         stop_token](result<value_t> status) mutable {
            if (stop_token.stop_requested() && status.has_value()) {
              status = result<value_t>::failure(errc::canceled);
            }
            std::invoke(handler, std::move(status));
          }));
      return;
    }
  }

private:
  static constexpr std::uint32_t async_spin_retry_limit_ = 64U;

  template <typename scheduler_t> struct push_wait_sender {
    using sender_concept = stdexec::sender_t;
    using completion_signatures =
        stdexec::completion_signatures<stdexec::set_value_t(result<void>)>;

    template <stdexec::receiver_of<completion_signatures> receiver_t>
    struct operation_state {
      queue_t *queue{nullptr};
      scheduler_t scheduler{};
      receiver_t receiver;
      value_t value;
      detail::turn_waiter waiter_state_{};
      std::atomic<bool> waiting_{false};
      std::atomic<bool> completed{false};
      std::atomic<bool> stop_requested{false};
      std::atomic<bool> scheduled_{false};
      std::atomic<bool> running_{false};

      struct stop_handler {
        operation_state *self{nullptr};
        void operator()() const noexcept {
          self->stop_requested.store(true, std::memory_order_release);
          self->schedule_attempt();
        }
      };
      using stop_token_t =
          stdexec::stop_token_of_t<stdexec::env_of_t<receiver_t>>;
      using stop_callback_t =
          stdexec::stop_callback_for_t<stop_token_t, stop_handler>;
      std::optional<stop_callback_t> on_stop_{};

      operation_state(queue_t *queue_ptr, scheduler_t scheduler_value,
                      receiver_t &&receiver_value,
                      value_t &&queued_value) noexcept
          : queue(queue_ptr), scheduler(std::move(scheduler_value)),
            receiver(std::forward<receiver_t>(receiver_value)),
            value(std::move(queued_value)) {}

      static void notify_waiter(void *owner,
                                detail::turn_waiter *waiter) noexcept {
        auto *self = static_cast<operation_state *>(owner);
        if (waiter != &(self->waiter_state_)) {
          return;
        }
        bool expected = true;
        if (self->waiting_.compare_exchange_strong(expected, false,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
          self->schedule_attempt();
        }
      }

      void start() & noexcept {
        if constexpr (!stdexec::unstoppable_token<stop_token_t>) {
          auto stop_token = stdexec::get_stop_token(stdexec::get_env(receiver));
          on_stop_.emplace(stop_token, stop_handler{this});
          if (stop_token.stop_requested()) {
            stop_requested.store(true, std::memory_order_release);
          }
        }
        schedule_attempt();
      }

      void schedule_attempt() noexcept {
        scheduled_.store(true, std::memory_order_release);
        if (running_.exchange(true, std::memory_order_acq_rel)) {
          return;
        }

        while (true) {
          scheduled_.store(false, std::memory_order_release);
          if (auto status = run_attempt(); status.has_value()) {
            complete(std::move(*status));
            return;
          }

          running_.store(false, std::memory_order_release);
          if (!scheduled_.load(std::memory_order_acquire) ||
              running_.exchange(true, std::memory_order_acq_rel)) {
            return;
          }
        }
      }

      [[nodiscard]] auto run_attempt() noexcept -> std::optional<result<void>> {
        if (completed.load(std::memory_order_acquire)) {
          return std::nullopt;
        }
        if (stop_requested.load(std::memory_order_acquire)) {
          return result<void>::failure(errc::canceled);
        }

        for (std::uint32_t attempt = 0U; attempt < async_spin_retry_limit_;
             ++attempt) {
          auto status = queue->try_push(static_cast<value_t &&>(value));
          if (status.has_value() || status.error() != errc::queue_full) {
            return status;
          }
          spin_pause();
        }

        auto status = queue->try_push(static_cast<value_t &&>(value));
        if (status.has_value() || status.error() != errc::queue_full) {
          return status;
        }

        const auto registration = queue->make_push_wait_registration();
        waiter_state_.turn_ptr = registration.turn_ptr;
        waiter_state_.expected_turn = registration.expected_turn;
        waiter_state_.channel_hint = registration.channel_hint;
        waiter_state_.channel_index = sender_notify::invalid_channel_index;
        waiter_state_.owner = this;
        waiter_state_.notify = &operation_state::notify_waiter;
        waiter_state_.next = nullptr;
        waiter_state_.prev = nullptr;
        waiting_.store(true, std::memory_order_release);

        if (!queue->arm_push_waiter(waiter_state_)) {
          waiting_.store(false, std::memory_order_release);
          scheduled_.store(true, std::memory_order_release);
        }
        return std::nullopt;
      }

      void complete(result<void> status) noexcept {
        if (completed.exchange(true, std::memory_order_acq_rel)) {
          return;
        }
        if (waiting_.exchange(false, std::memory_order_acq_rel)) {
          queue->disarm_push_waiter(waiter_state_);
        }
        stdexec::set_value(std::move(receiver), std::move(status));
      }
    };

    template <stdexec::receiver_of<completion_signatures> receiver_t>
    [[nodiscard]] auto connect(receiver_t receiver) && noexcept(
        std::is_nothrow_move_constructible_v<receiver_t> &&
        std::is_nothrow_move_constructible_v<scheduler_t> &&
        std::is_nothrow_move_constructible_v<value_t>)
        -> operation_state<receiver_t> {
      return operation_state<receiver_t>{queue, std::move(scheduler),
                                         std::move(receiver), std::move(value)};
    }

    template <stdexec::receiver_of<completion_signatures> receiver_t>
    [[nodiscard]] auto
    connect(receiver_t receiver) const & -> operation_state<receiver_t>
      requires std::copy_constructible<scheduler_t> &&
               std::copy_constructible<value_t>
    {
      return operation_state<receiver_t>{queue, scheduler, std::move(receiver),
                                         value};
    }

    queue_t *queue{nullptr};
    scheduler_t scheduler{};
    value_t value;
  };

  template <typename scheduler_t> struct pop_wait_sender {
    using sender_concept = stdexec::sender_t;
    using completion_signatures =
        stdexec::completion_signatures<stdexec::set_value_t(result<value_t>)>;

    template <stdexec::receiver_of<completion_signatures> receiver_t>
    struct operation_state {
      queue_t *queue{nullptr};
      scheduler_t scheduler{};
      receiver_t receiver;
      detail::turn_waiter waiter_state_{};
      std::atomic<bool> waiting_{false};
      std::atomic<bool> completed{false};
      std::atomic<bool> stop_requested{false};
      std::atomic<bool> scheduled_{false};
      std::atomic<bool> running_{false};

      struct stop_handler {
        operation_state *self{nullptr};
        void operator()() const noexcept {
          self->stop_requested.store(true, std::memory_order_release);
          self->schedule_attempt();
        }
      };
      using stop_token_t =
          stdexec::stop_token_of_t<stdexec::env_of_t<receiver_t>>;
      using stop_callback_t =
          stdexec::stop_callback_for_t<stop_token_t, stop_handler>;
      std::optional<stop_callback_t> on_stop_{};

      operation_state(queue_t *queue_ptr, scheduler_t scheduler_value,
                      receiver_t &&receiver_value) noexcept
          : queue(queue_ptr), scheduler(std::move(scheduler_value)),
            receiver(std::forward<receiver_t>(receiver_value)) {}

      static void notify_waiter(void *owner,
                                detail::turn_waiter *waiter) noexcept {
        auto *self = static_cast<operation_state *>(owner);
        if (waiter != &(self->waiter_state_)) {
          return;
        }
        bool expected = true;
        if (self->waiting_.compare_exchange_strong(expected, false,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
          self->schedule_attempt();
        }
      }

      void start() & noexcept {
        if constexpr (!stdexec::unstoppable_token<stop_token_t>) {
          auto stop_token = stdexec::get_stop_token(stdexec::get_env(receiver));
          on_stop_.emplace(stop_token, stop_handler{this});
          if (stop_token.stop_requested()) {
            stop_requested.store(true, std::memory_order_release);
          }
        }
        schedule_attempt();
      }

      void schedule_attempt() noexcept {
        scheduled_.store(true, std::memory_order_release);
        if (running_.exchange(true, std::memory_order_acq_rel)) {
          return;
        }

        while (true) {
          scheduled_.store(false, std::memory_order_release);
          if (auto status = run_attempt(); status.has_value()) {
            complete(std::move(*status));
            return;
          }

          running_.store(false, std::memory_order_release);
          if (!scheduled_.load(std::memory_order_acquire) ||
              running_.exchange(true, std::memory_order_acq_rel)) {
            return;
          }
        }
      }

      [[nodiscard]] auto run_attempt() noexcept
          -> std::optional<result<value_t>> {
        if (completed.load(std::memory_order_acquire)) {
          return std::nullopt;
        }
        if (stop_requested.load(std::memory_order_acquire)) {
          return result<value_t>::failure(errc::canceled);
        }

        for (std::uint32_t attempt = 0U; attempt < async_spin_retry_limit_;
             ++attempt) {
          auto status = queue->try_pop();
          if (status.has_value() || status.error() != errc::queue_empty) {
            return status;
          }
          spin_pause();
        }

        auto status = queue->try_pop();
        if (status.has_value() || status.error() != errc::queue_empty) {
          return status;
        }

        const auto registration = queue->make_pop_wait_registration();
        waiter_state_.turn_ptr = registration.turn_ptr;
        waiter_state_.expected_turn = registration.expected_turn;
        waiter_state_.channel_hint = registration.channel_hint;
        waiter_state_.channel_index = sender_notify::invalid_channel_index;
        waiter_state_.owner = this;
        waiter_state_.notify = &operation_state::notify_waiter;
        waiter_state_.next = nullptr;
        waiter_state_.prev = nullptr;
        waiting_.store(true, std::memory_order_release);

        if (!queue->arm_pop_waiter(waiter_state_)) {
          waiting_.store(false, std::memory_order_release);
          scheduled_.store(true, std::memory_order_release);
        }
        return std::nullopt;
      }

      void complete(result<value_t> status) noexcept {
        if (completed.exchange(true, std::memory_order_acq_rel)) {
          return;
        }
        if (waiting_.exchange(false, std::memory_order_acq_rel)) {
          queue->disarm_pop_waiter(waiter_state_);
        }
        stdexec::set_value(std::move(receiver), std::move(status));
      }
    };

    template <stdexec::receiver_of<completion_signatures> receiver_t>
    [[nodiscard]] auto connect(receiver_t receiver) && noexcept(
        std::is_nothrow_move_constructible_v<receiver_t> &&
        std::is_nothrow_move_constructible_v<scheduler_t>)
        -> operation_state<receiver_t> {
      return operation_state<receiver_t>{queue, std::move(scheduler),
                                         std::move(receiver)};
    }

    template <stdexec::receiver_of<completion_signatures> receiver_t>
    [[nodiscard]] auto
    connect(receiver_t receiver) const & -> operation_state<receiver_t>
      requires std::copy_constructible<scheduler_t>
    {
      return operation_state<receiver_t>{queue, scheduler, std::move(receiver)};
    }

    queue_t *queue{nullptr};
    scheduler_t scheduler{};
  };

  template <typename context_t>
  [[nodiscard]] auto make_push_sender(context_t context, value_t value) {
    using scheduler_t =
        std::remove_cvref_t<decltype(select_execution_scheduler(context))>;
    return push_wait_sender<scheduler_t>{static_cast<queue_t *>(this),
                                         select_execution_scheduler(context),
                                         std::move(value)};
  }

  template <typename context_t>
  [[nodiscard]] auto make_pop_sender(context_t context) {
    using scheduler_t =
        std::remove_cvref_t<decltype(select_execution_scheduler(context))>;
    return pop_wait_sender<scheduler_t>{static_cast<queue_t *>(this),
                                        select_execution_scheduler(context)};
  }
};

} // namespace detail

template <typename value_t, typename allocator_t>
class mpmc_queue<value_t, false, allocator_t>
    : public detail::mpmc_queue_async_facade<
          mpmc_queue<value_t, false, allocator_t>, value_t> {
public:
  using value_type = value_t;
  using allocator_type = allocator_t;
  using common_t = detail::mpmc_queue_common<value_t>;
  using slot = typename common_t::slot;
  using slot_allocator_t = typename std::allocator_traits<
      allocator_type>::template rebind_alloc<slot>;
  using slot_allocator_traits = std::allocator_traits<slot_allocator_t>;
  friend class detail::mpmc_queue_async_facade<
      mpmc_queue<value_t, false, allocator_t>, value_t>;

  static_assert(std::is_nothrow_move_constructible_v<value_t>,
                "mpmc_queue requires nothrow move constructible value type");
  static_assert(std::is_nothrow_destructible_v<value_t>,
                "mpmc_queue requires nothrow destructible value type");

  explicit mpmc_queue(const std::size_t capacity)
      : mpmc_queue(capacity, allocator_type{}) {}

  explicit mpmc_queue(const std::size_t capacity,
                      const allocator_type &allocator)
      : capacity_(common_t::validate_capacity(capacity)),
        stride_(common_t::compute_stride(capacity_)), allocator_(allocator) {
    if (is_power_of_two(capacity_)) {
      capacity_is_pow2_ = true;
      capacity_mask_ = capacity_ - 1U;
      capacity_shift_ = static_cast<unsigned>(std::countr_zero(capacity_));
    }
    init_slots();
  }

  mpmc_queue(const mpmc_queue &) = delete;
  auto operator=(const mpmc_queue &) -> mpmc_queue & = delete;
  mpmc_queue(mpmc_queue &&) = delete;
  auto operator=(mpmc_queue &&) -> mpmc_queue & = delete;

  ~mpmc_queue() {
    while (true) {
      auto value = try_pop();
      if (!value.has_value()) {
        break;
      }
    }
    destroy_slots();
  }

  template <typename... args_t>
    requires std::constructible_from<value_t, args_t &&...> &&
             std::is_nothrow_constructible_v<value_t, args_t &&...>
  [[nodiscard]] auto try_push(args_t &&...args) noexcept -> result<void> {
    std::uint64_t ticket = push_ticket_.load(std::memory_order_relaxed);
    while (true) {
      slot &target = slot_for_ticket(ticket);
      const std::uint64_t expected_turn = enqueue_turn_for_ticket(ticket);

      if (target.turn.load(std::memory_order_acquire) != expected_turn) {
        const std::uint64_t observed = ticket;
        ticket = push_ticket_.load(std::memory_order_relaxed);
        if (observed == ticket) {
          return result<void>::failure(errc::queue_full);
        }
        continue;
      }

      if (!push_ticket_.compare_exchange_weak(ticket, ticket + 1U,
                                              std::memory_order_relaxed,
                                              std::memory_order_relaxed)) {
        continue;
      }

      std::construct_at(target.value_ptr(), std::forward<args_t>(args)...);
      const auto publish_turn = expected_turn + 1U;
      target.turn.store(publish_turn, std::memory_order_release);
      notify_pop_waiters(&(target.turn), publish_turn);

      return result<void>::success();
    }
  }

  [[nodiscard]] auto try_pop() noexcept -> result<value_t>
    requires std::is_nothrow_move_constructible_v<value_t>
  {
    std::uint64_t ticket = pop_ticket_.load(std::memory_order_relaxed);
    while (true) {
      slot &target = slot_for_ticket(ticket);
      const std::uint64_t expected_turn = dequeue_turn_for_ticket(ticket);

      if (target.turn.load(std::memory_order_acquire) != expected_turn) {
        const std::uint64_t observed = ticket;
        ticket = pop_ticket_.load(std::memory_order_relaxed);
        if (observed == ticket) {
          return result<value_t>::failure(errc::queue_empty);
        }
        continue;
      }

      if (!pop_ticket_.compare_exchange_weak(ticket, ticket + 1U,
                                             std::memory_order_relaxed,
                                             std::memory_order_relaxed)) {
        continue;
      }

      value_t value = std::move(*target.value_ptr());
      std::destroy_at(target.value_ptr());
      const auto publish_turn = expected_turn + 1U;
      target.turn.store(publish_turn, std::memory_order_release);
      notify_push_waiters(&(target.turn), publish_turn);

      return result<value_t>::success(std::move(value));
    }
  }

  [[nodiscard]] auto empty() const noexcept -> bool {
    return write_count() == read_count();
  }

  [[nodiscard]] auto is_empty() const noexcept -> bool { return empty(); }

  [[nodiscard]] auto is_full() const noexcept -> bool {
    return approximate_depth() >= capacity_;
  }

  [[nodiscard]] auto size_guess() const noexcept -> std::int64_t {
    return static_cast<std::int64_t>(write_count()) -
           static_cast<std::int64_t>(read_count());
  }

  [[nodiscard]] auto lock_free() const noexcept -> bool {
    return push_ticket_.is_lock_free() && pop_ticket_.is_lock_free() &&
           slot_at_index(0U).turn.is_lock_free();
  }

  [[nodiscard]] auto push_count() const noexcept -> std::uint64_t {
    return write_count();
  }

  [[nodiscard]] auto pop_count() const noexcept -> std::uint64_t {
    return read_count();
  }

  [[nodiscard]] auto approximate_depth() const noexcept -> std::size_t {
    const auto writes = write_count();
    const auto reads = read_count();
    return static_cast<std::size_t>(writes - reads);
  }

  [[nodiscard]] auto capacity() const noexcept -> std::size_t {
    return capacity_;
  }

  [[nodiscard]] auto max_capacity() const noexcept -> std::size_t {
    return capacity_;
  }

  [[nodiscard]] auto allocated_capacity() const noexcept -> std::size_t {
    return capacity_;
  }

  [[nodiscard]] auto write_count() const noexcept -> std::uint64_t {
    return push_ticket_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] auto read_count() const noexcept -> std::uint64_t {
    return pop_ticket_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] auto get_allocator() const noexcept -> allocator_type {
    return allocator_;
  }

  [[nodiscard]] constexpr auto dynamic_growth_enabled() const noexcept -> bool {
    return false;
  }

private:
  static constexpr bool using_std_allocator =
      std::is_same_v<allocator_type, std::allocator<value_type>>;

  [[nodiscard]] auto make_push_wait_registration() noexcept
      -> detail::turn_wait_registration {
    const auto ticket = push_ticket_.load(std::memory_order_relaxed);
    const auto index = slot_index(ticket);
    auto *turn_ptr = &(slots_[index].turn);
    const auto expected_turn = enqueue_turn_for_ticket(ticket);
    return detail::turn_wait_registration{
        turn_ptr, expected_turn,
        sender_notify::suggest_channel_index(turn_ptr, expected_turn)};
  }

  [[nodiscard]] auto make_pop_wait_registration() noexcept
      -> detail::turn_wait_registration {
    const auto ticket = pop_ticket_.load(std::memory_order_relaxed);
    const auto index = slot_index(ticket);
    auto *turn_ptr = &(slots_[index].turn);
    const auto expected_turn = dequeue_turn_for_ticket(ticket);
    return detail::turn_wait_registration{
        turn_ptr, expected_turn,
        sender_notify::suggest_channel_index(turn_ptr, expected_turn)};
  }

  [[nodiscard]] auto arm_push_waiter(detail::turn_waiter &waiter) noexcept
      -> bool {
    return push_wait_notify_.arm(waiter);
  }

  void disarm_push_waiter(detail::turn_waiter &waiter) noexcept {
    push_wait_notify_.disarm(waiter);
  }

  [[nodiscard]] auto arm_pop_waiter(detail::turn_waiter &waiter) noexcept
      -> bool {
    return pop_wait_notify_.arm(waiter);
  }

  void disarm_pop_waiter(detail::turn_waiter &waiter) noexcept {
    pop_wait_notify_.disarm(waiter);
  }

  void notify_push_waiters(std::atomic<std::uint64_t> *turn_ptr,
                           const std::uint64_t turn_value) noexcept {
    push_wait_notify_.notify(turn_ptr, turn_value);
  }

  void notify_pop_waiters(std::atomic<std::uint64_t> *turn_ptr,
                          const std::uint64_t turn_value) noexcept {
    pop_wait_notify_.notify(turn_ptr, turn_value);
  }

  [[nodiscard]] auto
  enqueue_turn_for_ticket(const std::uint64_t ticket) const noexcept
      -> std::uint64_t {
    if (capacity_is_pow2_) {
      return (ticket >> capacity_shift_) << 1U;
    }
    return (ticket / capacity_) * 2U;
  }

  [[nodiscard]] auto
  dequeue_turn_for_ticket(const std::uint64_t ticket) const noexcept
      -> std::uint64_t {
    return enqueue_turn_for_ticket(ticket) + 1U;
  }

  void init_slots() {
    const auto count = common_t::slot_count(capacity_);
    if constexpr (using_std_allocator) {
      slots_ = new (std::nothrow) slot[count];
      wh_precondition(slots_ != nullptr);
    } else {
      auto slot_allocator = slot_allocator_t{allocator_};
      slots_ = slot_allocator_traits::allocate(slot_allocator, count);
      for (std::size_t index = 0U; index < count; ++index) {
        slot_allocator_traits::construct(slot_allocator, slots_ + index);
      }
    }

    for (std::size_t index = 0U; index < capacity_; ++index) {
      slot_at_index(index).turn.store(0U, std::memory_order_relaxed);
    }
  }

  void destroy_slots() noexcept {
    if (slots_ == nullptr) {
      return;
    }

    const auto count = common_t::slot_count(capacity_);
    if constexpr (using_std_allocator) {
      delete[] slots_;
    } else {
      auto slot_allocator = slot_allocator_t{allocator_};
      for (std::size_t index = 0U; index < count; ++index) {
        slot_allocator_traits::destroy(slot_allocator, slots_ + index);
      }
      slot_allocator_traits::deallocate(slot_allocator, slots_, count);
    }
    slots_ = nullptr;
  }

  [[nodiscard]] auto slot_index(const std::uint64_t ticket) const noexcept
      -> std::size_t {
    const auto stride = static_cast<std::uint64_t>(stride_);
    if (capacity_is_pow2_) {
      return static_cast<std::size_t>((ticket * stride) & capacity_mask_) +
             common_t::slot_padding;
    }
    return static_cast<std::size_t>((ticket * stride) % capacity_) +
           common_t::slot_padding;
  }

  [[nodiscard]] auto slot_for_ticket(const std::uint64_t ticket) noexcept
      -> slot & {
    return slots_[slot_index(ticket)];
  }

  [[nodiscard]] auto slot_at_index(const std::size_t index) noexcept -> slot & {
    return slots_[index + common_t::slot_padding];
  }

  [[nodiscard]] auto slot_at_index(const std::size_t index) const noexcept
      -> const slot & {
    return slots_[index + common_t::slot_padding];
  }

  const std::size_t capacity_;
  const int stride_;
  allocator_type allocator_{};
  slot *slots_{nullptr};
  bool capacity_is_pow2_{false};
  unsigned capacity_shift_{0U};
  std::size_t capacity_mask_{0U};

  alignas(default_cacheline_size) std::atomic<std::uint64_t> push_ticket_{0U};
  alignas(default_cacheline_size) std::atomic<std::uint64_t> pop_ticket_{0U};
  sender_notify push_wait_notify_{};
  sender_notify pop_wait_notify_{};
};

template <typename value_t, typename allocator_t>
class mpmc_queue<value_t, true, allocator_t>
    : public detail::mpmc_queue_async_facade<
          mpmc_queue<value_t, true, allocator_t>, value_t> {
public:
  using value_type = value_t;
  using allocator_type = allocator_t;
  using common_t = detail::mpmc_queue_common<value_t>;
  using slot = typename common_t::slot;
  friend class detail::mpmc_queue_async_facade<
      mpmc_queue<value_t, true, allocator_t>, value_t>;

  static_assert(std::is_same_v<allocator_type, std::allocator<value_type>>,
                "dynamic mpmc_queue aligns with Folly: only std::allocator "
                "is supported");
  static_assert(std::is_nothrow_move_constructible_v<value_t>,
                "mpmc_queue requires nothrow move constructible value type");
  static_assert(std::is_nothrow_destructible_v<value_t>,
                "mpmc_queue requires nothrow destructible value type");

  static constexpr std::size_t default_min_dynamic_capacity = 10U;
  static constexpr std::size_t default_expansion_multiplier = 10U;

  explicit mpmc_queue(const std::size_t initial_capacity)
      : mpmc_queue(
            normalize_initial_capacity(
                common_t::validate_capacity(initial_capacity),
                default_min_dynamic_capacity),
            make_dynamic_options(common_t::validate_capacity(initial_capacity),
                                 default_expansion_multiplier)) {}

  explicit mpmc_queue(const std::size_t queue_capacity,
                      const std::size_t min_capacity,
                      const std::size_t expansion_multiplier)
      : mpmc_queue(
            normalize_initial_capacity(
                common_t::validate_capacity(queue_capacity), min_capacity),
            make_dynamic_options(common_t::validate_capacity(queue_capacity),
                                 expansion_multiplier)) {}

  explicit mpmc_queue(const std::size_t initial_capacity,
                      const mpmc_dynamic_options options)
      : max_capacity_(resolve_max_capacity(initial_capacity, options)),
        growth_factor_(resolve_growth_factor(options.growth_factor)),
        max_closed_arrays_(compute_max_closed_arrays(
            common_t::validate_capacity(initial_capacity), max_capacity_,
            growth_factor_)),
        closed_arrays_(max_closed_arrays_ > 0U
                           ? new (std::nothrow) closed_array[max_closed_arrays_]
                           : nullptr) {
    wh_precondition(max_closed_arrays_ == 0U || closed_arrays_ != nullptr);

    const std::size_t capacity = common_t::validate_capacity(initial_capacity);
    slot *slots = allocate_slots(capacity);
    wh_precondition(slots != nullptr);

    dslots_.store(slots, std::memory_order_relaxed);
    dcapacity_.store(capacity, std::memory_order_relaxed);
    dstride_.store(common_t::compute_stride(capacity),
                   std::memory_order_relaxed);
    dstate_.store(0U, std::memory_order_relaxed);
  }

  mpmc_queue(const mpmc_queue &) = delete;
  auto operator=(const mpmc_queue &) -> mpmc_queue & = delete;
  mpmc_queue(mpmc_queue &&) = delete;
  auto operator=(mpmc_queue &&) -> mpmc_queue & = delete;

  ~mpmc_queue() {
    while (true) {
      auto value = try_pop();
      if (!value.has_value()) {
        break;
      }
    }
    destroy_dynamic_storage();
  }

  template <typename... args_t>
    requires std::constructible_from<value_t, args_t &&...> &&
             std::is_nothrow_constructible_v<value_t, args_t &&...>
  [[nodiscard]] auto try_push(args_t &&...args) noexcept -> result<void> {
    if (approximate_depth() >= max_capacity_) {
      return result<void>::failure(errc::queue_full);
    }

    std::uint64_t ticket = 0U;
    while (true) {
      ticket = push_ticket_.load(std::memory_order_relaxed);

      slot *slots = nullptr;
      std::size_t capacity = 0U;
      int stride = 1;
      std::uint64_t state = 0U;
      if (!try_seqlock_read_section(state, slots, capacity, stride)) {
        spin_pause();
        continue;
      }

      std::uint64_t offset = 0U;
      const auto used_closed_array = maybe_update_from_closed(
          state, ticket, offset, slots, capacity, stride);
      (void)used_closed_array;

      const std::uint64_t local_ticket = ticket - offset;
      slot &target_slot =
          slot_for_ticket(slots, capacity, stride, local_ticket);
      const std::uint64_t expected_turn =
          common_t::enqueue_turn(local_ticket, capacity);

      if (target_slot.turn.load(std::memory_order_acquire) == expected_turn) {
        if (!push_ticket_.compare_exchange_strong(ticket, ticket + 1U,
                                                  std::memory_order_relaxed,
                                                  std::memory_order_relaxed)) {
          continue;
        }

        std::construct_at(target_slot.value_ptr(),
                          std::forward<args_t>(args)...);
        const auto publish_turn = expected_turn + 1U;
        target_slot.turn.store(publish_turn, std::memory_order_release);
        notify_pop_waiters(&(target_slot.turn), publish_turn);

        return result<void>::success();
      }

      if (ticket != push_ticket_.load(std::memory_order_relaxed)) {
        continue;
      }

      if (offset == get_offset(state) && try_expand(state, capacity)) {
        continue;
      }
      return result<void>::failure(errc::queue_full);
    }
  }

  [[nodiscard]] auto try_pop() noexcept -> result<value_t>
    requires std::is_nothrow_move_constructible_v<value_t>
  {
    std::uint64_t ticket = 0U;
    while (true) {
      ticket = pop_ticket_.load(std::memory_order_relaxed);

      slot *slots = nullptr;
      std::size_t capacity = 0U;
      int stride = 1;
      std::uint64_t state = 0U;
      if (!try_seqlock_read_section(state, slots, capacity, stride)) {
        spin_pause();
        continue;
      }

      std::uint64_t offset = 0U;
      const auto used_closed_array = maybe_update_from_closed(
          state, ticket, offset, slots, capacity, stride);
      (void)used_closed_array;

      const std::uint64_t local_ticket = ticket - offset;
      slot &target_slot =
          slot_for_ticket(slots, capacity, stride, local_ticket);
      const std::uint64_t expected_turn =
          common_t::dequeue_turn(local_ticket, capacity);

      if (target_slot.turn.load(std::memory_order_acquire) != expected_turn) {
        return result<value_t>::failure(errc::queue_empty);
      }

      if (!pop_ticket_.compare_exchange_strong(ticket, ticket + 1U,
                                               std::memory_order_relaxed,
                                               std::memory_order_relaxed)) {
        continue;
      }

      value_t value = std::move(*target_slot.value_ptr());
      std::destroy_at(target_slot.value_ptr());
      const auto publish_turn = expected_turn + 1U;
      target_slot.turn.store(publish_turn, std::memory_order_release);
      notify_push_waiters(&(target_slot.turn), publish_turn);

      return result<value_t>::success(std::move(value));
    }
  }

  [[nodiscard]] auto empty() const noexcept -> bool {
    return write_count() == read_count();
  }

  [[nodiscard]] auto is_empty() const noexcept -> bool { return empty(); }

  [[nodiscard]] auto is_full() const noexcept -> bool {
    return approximate_depth() >= max_capacity_;
  }

  [[nodiscard]] auto size_guess() const noexcept -> std::int64_t {
    return static_cast<std::int64_t>(write_count()) -
           static_cast<std::int64_t>(read_count());
  }

  [[nodiscard]] auto lock_free() const noexcept -> bool {
    const auto *active_slots = dslots_.load(std::memory_order_acquire);
    const auto active_capacity = dcapacity_.load(std::memory_order_relaxed);
    if (active_slots == nullptr || active_capacity == 0U) {
      return false;
    }
    return push_ticket_.is_lock_free() && pop_ticket_.is_lock_free() &&
           dstate_.is_lock_free() &&
           slot_at_index(active_slots, 0U).turn.is_lock_free();
  }

  [[nodiscard]] auto push_count() const noexcept -> std::uint64_t {
    return write_count();
  }

  [[nodiscard]] auto pop_count() const noexcept -> std::uint64_t {
    return read_count();
  }

  [[nodiscard]] auto approximate_depth() const noexcept -> std::size_t {
    const auto writes = write_count();
    const auto reads = read_count();
    return static_cast<std::size_t>(writes - reads);
  }

  [[nodiscard]] auto capacity() const noexcept -> std::size_t {
    return dcapacity_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] auto max_capacity() const noexcept -> std::size_t {
    return max_capacity_;
  }

  [[nodiscard]] auto allocated_capacity() const noexcept -> std::size_t {
    return dcapacity_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] auto write_count() const noexcept -> std::uint64_t {
    return push_ticket_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] auto read_count() const noexcept -> std::uint64_t {
    return pop_ticket_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] auto get_allocator() const noexcept -> allocator_type {
    return allocator_type{};
  }

  [[nodiscard]] constexpr auto dynamic_growth_enabled() const noexcept -> bool {
    return true;
  }

private:
  struct closed_array {
    std::uint64_t offset{0U};
    slot *slots{nullptr};
    std::size_t capacity{0U};
    int stride{1};
  };

  static constexpr std::uint8_t seqlock_bits = 8U;
  static constexpr std::uint64_t seqlock_mask = (1ULL << seqlock_bits) - 1ULL;

  [[nodiscard]] auto make_push_wait_registration() noexcept
      -> detail::turn_wait_registration {
    const auto ticket = push_ticket_.load(std::memory_order_relaxed);
    return make_wait_registration_for_ticket(ticket, true);
  }

  [[nodiscard]] auto make_pop_wait_registration() noexcept
      -> detail::turn_wait_registration {
    const auto ticket = pop_ticket_.load(std::memory_order_relaxed);
    return make_wait_registration_for_ticket(ticket, false);
  }

  [[nodiscard]] auto arm_push_waiter(detail::turn_waiter &waiter) noexcept
      -> bool {
    return push_wait_notify_.arm(waiter);
  }

  void disarm_push_waiter(detail::turn_waiter &waiter) noexcept {
    push_wait_notify_.disarm(waiter);
  }

  [[nodiscard]] auto arm_pop_waiter(detail::turn_waiter &waiter) noexcept
      -> bool {
    return pop_wait_notify_.arm(waiter);
  }

  void disarm_pop_waiter(detail::turn_waiter &waiter) noexcept {
    pop_wait_notify_.disarm(waiter);
  }

  void notify_push_waiters(std::atomic<std::uint64_t> *turn_ptr,
                           const std::uint64_t turn_value) noexcept {
    push_wait_notify_.notify(turn_ptr, turn_value);
  }

  void notify_pop_waiters(std::atomic<std::uint64_t> *turn_ptr,
                          const std::uint64_t turn_value) noexcept {
    pop_wait_notify_.notify(turn_ptr, turn_value);
  }

  [[nodiscard]] auto
  make_wait_registration_for_ticket(const std::uint64_t ticket,
                                    const bool producer_wait) noexcept
      -> detail::turn_wait_registration {
    while (true) {
      slot *slots = nullptr;
      std::size_t capacity = 0U;
      int stride = 1;
      std::uint64_t state = 0U;
      if (!try_seqlock_read_section(state, slots, capacity, stride)) {
        spin_pause();
        continue;
      }

      std::uint64_t offset = 0U;
      const auto used_closed_array = maybe_update_from_closed(
          state, ticket, offset, slots, capacity, stride);
      (void)used_closed_array;

      const auto local_ticket = ticket - offset;
      const auto index = slot_index(local_ticket, capacity, stride);
      const auto expected_turn =
          producer_wait ? common_t::enqueue_turn(local_ticket, capacity)
                        : common_t::dequeue_turn(local_ticket, capacity);
      return detail::turn_wait_registration{
          &(slots[index].turn), expected_turn,
          sender_notify::suggest_channel_index(&(slots[index].turn),
                                               expected_turn)};
    }
  }

  [[nodiscard]] static auto
  resolve_growth_factor(const std::size_t growth_factor) noexcept
      -> std::size_t {
    return growth_factor < 2U ? 2U : growth_factor;
  }

  [[nodiscard]] static auto
  resolve_max_capacity(const std::size_t initial_capacity,
                       const mpmc_dynamic_options options) noexcept
      -> std::size_t {
    if (options.max_capacity == 0U) {
      return initial_capacity;
    }
    return std::max(options.max_capacity, initial_capacity);
  }

  [[nodiscard]] static auto compute_max_closed_arrays(
      const std::size_t initial_capacity, const std::size_t max_capacity,
      const std::size_t growth_factor) noexcept -> std::size_t {
    if (initial_capacity >= max_capacity) {
      return 0U;
    }

    std::size_t count = 0U;
    std::size_t expanded = initial_capacity;
    while (expanded < max_capacity) {
      if (expanded > (max_capacity / growth_factor)) {
        expanded = max_capacity;
      } else {
        expanded *= growth_factor;
      }
      ++count;
    }
    return count;
  }

  [[nodiscard]] static auto
  normalize_initial_capacity(const std::size_t queue_capacity,
                             const std::size_t min_capacity) noexcept
      -> std::size_t {
    return std::min(std::max<std::size_t>(1U, min_capacity), queue_capacity);
  }

  [[nodiscard]] static auto
  make_dynamic_options(const std::size_t queue_capacity,
                       const std::size_t expansion_multiplier) noexcept
      -> mpmc_dynamic_options {
    mpmc_dynamic_options options{};
    options.max_capacity = queue_capacity;
    options.growth_factor = expansion_multiplier;
    return options;
  }

  [[nodiscard]] static auto allocate_slots(const std::size_t capacity) noexcept
      -> slot * {
    slot *slots = new (std::nothrow) slot[common_t::slot_count(capacity)];
    if (slots == nullptr) {
      return nullptr;
    }

    for (std::size_t index = 0U; index < capacity; ++index) {
      slots[index + common_t::slot_padding].turn.store(
          0U, std::memory_order_relaxed);
    }
    return slots;
  }

  void destroy_dynamic_storage() noexcept {
    const std::uint64_t state = dstate_.load(std::memory_order_relaxed);
    const auto num_closed = get_num_closed(state);

    for (std::size_t index = num_closed; index > 0U; --index) {
      delete[] closed_arrays_[index - 1U].slots;
    }
    delete[] closed_arrays_;
    closed_arrays_ = nullptr;

    slot *active_slots = dslots_.load(std::memory_order_relaxed);
    delete[] active_slots;
    dslots_.store(nullptr, std::memory_order_relaxed);
    dcapacity_.store(0U, std::memory_order_relaxed);
    dstride_.store(0, std::memory_order_relaxed);
    dstate_.store(0U, std::memory_order_relaxed);
  }

  [[nodiscard]] static auto get_offset(const std::uint64_t state) noexcept
      -> std::uint64_t {
    return state >> seqlock_bits;
  }

  [[nodiscard]] static auto get_num_closed(const std::uint64_t state) noexcept
      -> std::size_t {
    return static_cast<std::size_t>((state & seqlock_mask) >> 1U);
  }

  [[nodiscard]] auto next_capacity(const std::size_t current) const noexcept
      -> std::size_t {
    if (current >= max_capacity_) {
      return current;
    }

    std::size_t grown = current;
    if (grown > (max_capacity_ / growth_factor_)) {
      grown = max_capacity_;
    } else {
      grown *= growth_factor_;
    }
    if (grown <= current) {
      return max_capacity_;
    }
    return std::min(grown, max_capacity_);
  }

  [[nodiscard]] auto
  try_seqlock_read_section(std::uint64_t &state, slot *&slots,
                           std::size_t &capacity, int &stride) const noexcept
      -> bool {
    state = dstate_.load(std::memory_order_acquire);
    if ((state & 1ULL) != 0ULL) {
      return false;
    }

    slots = dslots_.load(std::memory_order_relaxed);
    capacity = dcapacity_.load(std::memory_order_relaxed);
    stride = dstride_.load(std::memory_order_relaxed);

    std::atomic_thread_fence(std::memory_order_acquire);
    return state == dstate_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] auto
  maybe_update_from_closed(const std::uint64_t state,
                           const std::uint64_t ticket, std::uint64_t &offset,
                           slot *&slots, std::size_t &capacity,
                           int &stride) const noexcept -> bool {
    offset = get_offset(state);
    if (ticket >= offset) {
      return false;
    }

    const auto num_closed = get_num_closed(state);
    for (std::size_t index = num_closed; index > 0U; --index) {
      const auto &closed = closed_arrays_[index - 1U];
      if (closed.offset <= ticket) {
        offset = closed.offset;
        slots = closed.slots;
        capacity = closed.capacity;
        stride = closed.stride;
        return true;
      }
    }

    wh_precondition(false);
    return false;
  }

  [[nodiscard]] auto try_expand(const std::uint64_t state,
                                const std::size_t capacity) noexcept -> bool {
    if (capacity >= max_capacity_) {
      return false;
    }

    std::uint64_t expected = state;
    if (!dstate_.compare_exchange_strong(expected, state + 1U,
                                         std::memory_order_acq_rel,
                                         std::memory_order_relaxed)) {
      return true;
    }

    const auto expanded_capacity = next_capacity(capacity);
    if (expanded_capacity <= capacity) {
      dstate_.store(state, std::memory_order_release);
      return false;
    }

    slot *new_slots = allocate_slots(expanded_capacity);
    if (new_slots == nullptr) {
      dstate_.store(state, std::memory_order_release);
      return false;
    }

    const auto closed_index = get_num_closed(state);
    if (closed_index >= max_closed_arrays_) {
      delete[] new_slots;
      dstate_.store(state, std::memory_order_release);
      return false;
    }

    const auto ticket_offset =
        1U + std::max(push_ticket_.load(std::memory_order_relaxed),
                      pop_ticket_.load(std::memory_order_relaxed));

    closed_arrays_[closed_index].offset = get_offset(state);
    closed_arrays_[closed_index].slots =
        dslots_.load(std::memory_order_relaxed);
    closed_arrays_[closed_index].capacity = capacity;
    closed_arrays_[closed_index].stride =
        dstride_.load(std::memory_order_relaxed);

    dslots_.store(new_slots, std::memory_order_relaxed);
    dcapacity_.store(expanded_capacity, std::memory_order_relaxed);
    dstride_.store(common_t::compute_stride(expanded_capacity),
                   std::memory_order_relaxed);

    const auto new_state = (ticket_offset << seqlock_bits) +
                           static_cast<std::uint64_t>(2U * (closed_index + 1U));
    dstate_.store(new_state, std::memory_order_release);
    return true;
  }

  [[nodiscard]] static auto slot_index(const std::uint64_t local_ticket,
                                       const std::size_t capacity,
                                       const int stride) noexcept
      -> std::size_t {
    const auto stride_value = static_cast<std::uint64_t>(stride);
    if (is_power_of_two(capacity)) {
      return static_cast<std::size_t>((local_ticket * stride_value) &
                                      (capacity - 1U)) +
             common_t::slot_padding;
    }
    return static_cast<std::size_t>((local_ticket * stride_value) % capacity) +
           common_t::slot_padding;
  }

  [[nodiscard]] static auto
  slot_for_ticket(slot *slots, const std::size_t capacity, const int stride,
                  const std::uint64_t local_ticket) noexcept -> slot & {
    return slots[slot_index(local_ticket, capacity, stride)];
  }

  [[nodiscard]] static auto slot_at_index(const slot *slots,
                                          const std::size_t index) noexcept
      -> const slot & {
    return slots[index + common_t::slot_padding];
  }

  const std::size_t max_capacity_{0U};
  const std::size_t growth_factor_{2U};
  const std::size_t max_closed_arrays_{0U};
  closed_array *closed_arrays_{nullptr};

  std::atomic<slot *> dslots_{nullptr};
  std::atomic<int> dstride_{0};
  std::atomic<std::uint64_t> dstate_{0U};
  std::atomic<std::size_t> dcapacity_{0U};

  alignas(default_cacheline_size) std::atomic<std::uint64_t> push_ticket_{0U};
  alignas(default_cacheline_size) std::atomic<std::uint64_t> pop_ticket_{0U};
  sender_notify push_wait_notify_{};
  sender_notify pop_wait_notify_{};
};

} // namespace wh::core
