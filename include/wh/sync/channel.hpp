#pragma once

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include <exec/start_detached.hpp>
#include <exec/when_any.hpp>
#include <stdexec/execution.hpp>

#include "wh/async/async_initiate.hpp"
#include "wh/async/completion_token_helper.hpp"
#include "wh/core/compiler.hpp"
#include "wh/core/error.hpp"
#include "wh/core/mpmc_queue.hpp"
#include "wh/core/result.hpp"
#include "wh/scheduler/timer_helper.hpp"
#include "wh/sync/sender_notify.hpp"

namespace wh::core {

struct channel_options {
  std::size_t capacity{0U};
};

template <typename value_t, typename allocator_t = std::allocator<value_t>>
class channel {
private:
  struct state;

public:
  using value_type = value_t;
  using allocator_type = allocator_t;

  class sender;
  class receiver;

  explicit channel(const std::size_t capacity)
      : channel(channel_options{capacity}) {}

  explicit channel(const channel_options options)
      : channel(options, allocator_type{}) {}

  explicit channel(const channel_options options,
                   const allocator_type &allocator)
      : state_(std::make_shared<state>(options.capacity, allocator)) {
    wh_precondition(options.capacity > 0U);
  }

  channel(const channel &) = default;
  auto operator=(const channel &) -> channel & = default;
  channel(channel &&) noexcept = default;
  auto operator=(channel &&) noexcept -> channel & = default;
  ~channel() = default;

  template <typename... args_t>
    requires std::constructible_from<value_t, args_t &&...> &&
             std::is_nothrow_constructible_v<value_t, args_t &&...>
  [[nodiscard]] auto try_push(args_t &&...args) noexcept -> result<void> {
    return try_push_impl(state_, std::forward<args_t>(args)...);
  }

  [[nodiscard]] auto try_pop() noexcept -> result<value_t>
    requires std::is_nothrow_move_constructible_v<value_t>
  {
    return try_pop_impl(state_);
  }

  [[nodiscard]] auto close() noexcept -> bool { return close_impl(state_); }

  [[nodiscard]] auto is_closed() const noexcept -> bool {
    return state_->closed.load(std::memory_order_acquire);
  }

  [[nodiscard]] auto approximate_depth() const noexcept -> std::size_t {
    return state_->queue.approximate_depth();
  }

  [[nodiscard]] auto capacity() const noexcept -> std::size_t {
    return state_->queue.capacity();
  }

  [[nodiscard]] auto split() const -> std::pair<sender, receiver> {
    return {sender{state_}, receiver{state_}};
  }

  template <scheduler_context_like scheduler_context_t,
            completion_token completion_token_t = use_sender_t>
  [[nodiscard]] auto push(scheduler_context_t context, const value_t &value,
                          completion_token_t token = completion_token_t{})
      -> decltype(auto)
    requires std::is_nothrow_copy_constructible_v<value_t>
  {
    return dispatch_token<result<void>>(
        std::move(token), [state = state_, context, copied = value]() mutable {
          return make_push_sender(std::move(state), context, std::move(copied));
        });
  }

  template <scheduler_context_like scheduler_context_t,
            completion_token completion_token_t = use_sender_t>
  [[nodiscard]] auto push(scheduler_context_t context, value_t &&value,
                          completion_token_t token = completion_token_t{})
      -> decltype(auto)
    requires std::is_nothrow_move_constructible_v<value_t>
  {
    return dispatch_token<result<void>>(
        std::move(token), [state = state_, context, moved = std::move(value)]() mutable {
          return make_push_sender(std::move(state), context, std::move(moved));
        });
  }

  template <scheduler_context_like scheduler_context_t,
            completion_token completion_token_t = use_sender_t>
  [[nodiscard]] auto pop(scheduler_context_t context,
                         completion_token_t token = completion_token_t{})
      -> decltype(auto)
    requires std::is_nothrow_move_constructible_v<value_t>
  {
    return dispatch_token<result<value_t>>(
        std::move(token), [state = state_, context]() mutable {
          return make_pop_sender(std::move(state), context);
        });
  }

  template <timed_scheduler_in_context scheduler_context_t, typename deadline_t,
            completion_token completion_token_t = use_sender_t>
  [[nodiscard]] auto push_until(scheduler_context_t context,
                                const deadline_t &deadline,
                                const value_t &value,
                                completion_token_t token = completion_token_t{})
      -> decltype(auto)
    requires std::is_nothrow_copy_constructible_v<value_t>
  {
    return dispatch_token<result<void>>(
        std::move(token),
        [state = state_, context, deadline, copied = value]() mutable {
          return make_push_until_sender(std::move(state), context, deadline,
                                        std::move(copied));
        });
  }

  template <timed_scheduler_in_context scheduler_context_t, typename deadline_t,
            completion_token completion_token_t = use_sender_t>
  [[nodiscard]] auto push_until(scheduler_context_t context,
                                const deadline_t &deadline, value_t &&value,
                                completion_token_t token = completion_token_t{})
      -> decltype(auto)
    requires std::is_nothrow_move_constructible_v<value_t>
  {
    return dispatch_token<result<void>>(
        std::move(token),
        [state = state_, context, deadline, moved = std::move(value)]() mutable {
          return make_push_until_sender(std::move(state), context, deadline,
                                        std::move(moved));
        });
  }

  template <timed_scheduler_in_context scheduler_context_t, typename deadline_t,
            completion_token completion_token_t = use_sender_t>
  [[nodiscard]] auto pop_until(scheduler_context_t context,
                               const deadline_t &deadline,
                               completion_token_t token = completion_token_t{})
      -> decltype(auto)
    requires std::is_nothrow_move_constructible_v<value_t>
  {
    return dispatch_token<result<value_t>>(
        std::move(token), [state = state_, context, deadline]() mutable {
          return make_pop_until_sender(std::move(state), context, deadline);
        });
  }

  class sender {
  public:
    template <typename... args_t>
      requires std::constructible_from<value_t, args_t &&...> &&
               std::is_nothrow_constructible_v<value_t, args_t &&...>
    [[nodiscard]] auto try_push(args_t &&...args) noexcept -> result<void> {
      return try_push_impl(state_, std::forward<args_t>(args)...);
    }

    [[nodiscard]] auto close() noexcept -> bool { return close_impl(state_); }

    [[nodiscard]] auto is_closed() const noexcept -> bool {
      return state_->closed.load(std::memory_order_acquire);
    }

    [[nodiscard]] auto approximate_depth() const noexcept -> std::size_t {
      return state_->queue.approximate_depth();
    }

    [[nodiscard]] auto capacity() const noexcept -> std::size_t {
      return state_->queue.capacity();
    }

    template <scheduler_context_like scheduler_context_t,
              completion_token completion_token_t = use_sender_t>
    [[nodiscard]] auto push(scheduler_context_t context, const value_t &value,
                            completion_token_t token = completion_token_t{})
        -> decltype(auto)
      requires std::is_nothrow_copy_constructible_v<value_t>
    {
      return dispatch_token<result<void>>(
          std::move(token),
          [state = state_, context, copied = value]() mutable {
            return make_push_sender(std::move(state), context,
                                    std::move(copied));
          });
    }

    template <scheduler_context_like scheduler_context_t,
              completion_token completion_token_t = use_sender_t>
    [[nodiscard]] auto push(scheduler_context_t context, value_t &&value,
                            completion_token_t token = completion_token_t{})
        -> decltype(auto)
      requires std::is_nothrow_move_constructible_v<value_t>
    {
      return dispatch_token<result<void>>(
          std::move(token),
          [state = state_, context, moved = std::move(value)]() mutable {
            return make_push_sender(std::move(state), context, std::move(moved));
          });
    }

    template <timed_scheduler_in_context scheduler_context_t,
              typename deadline_t,
              completion_token completion_token_t = use_sender_t>
    [[nodiscard]] auto push_until(
        scheduler_context_t context, const deadline_t &deadline,
        const value_t &value, completion_token_t token = completion_token_t{})
        -> decltype(auto)
      requires std::is_nothrow_copy_constructible_v<value_t>
    {
      return dispatch_token<result<void>>(
          std::move(token),
          [state = state_, context, deadline, copied = value]() mutable {
            return make_push_until_sender(std::move(state), context, deadline,
                                          std::move(copied));
          });
    }

    template <timed_scheduler_in_context scheduler_context_t,
              typename deadline_t,
              completion_token completion_token_t = use_sender_t>
    [[nodiscard]] auto push_until(
        scheduler_context_t context, const deadline_t &deadline, value_t &&value,
        completion_token_t token = completion_token_t{}) -> decltype(auto)
      requires std::is_nothrow_move_constructible_v<value_t>
    {
      return dispatch_token<result<void>>(
          std::move(token),
          [state = state_, context, deadline, moved = std::move(value)]() mutable {
            return make_push_until_sender(std::move(state), context, deadline,
                                          std::move(moved));
          });
    }

  private:
    friend class channel;
    explicit sender(std::shared_ptr<state> state) : state_(std::move(state)) {}

    std::shared_ptr<state> state_{};
  };

  class receiver {
  public:
    [[nodiscard]] auto try_pop() noexcept -> result<value_t>
      requires std::is_nothrow_move_constructible_v<value_t>
    {
      return try_pop_impl(state_);
    }

    [[nodiscard]] auto is_closed() const noexcept -> bool {
      return state_->closed.load(std::memory_order_acquire);
    }

    [[nodiscard]] auto approximate_depth() const noexcept -> std::size_t {
      return state_->queue.approximate_depth();
    }

    [[nodiscard]] auto capacity() const noexcept -> std::size_t {
      return state_->queue.capacity();
    }

    template <scheduler_context_like scheduler_context_t,
              completion_token completion_token_t = use_sender_t>
    [[nodiscard]] auto pop(scheduler_context_t context,
                           completion_token_t token = completion_token_t{})
        -> decltype(auto)
      requires std::is_nothrow_move_constructible_v<value_t>
    {
      return dispatch_token<result<value_t>>(
          std::move(token), [state = state_, context]() mutable {
            return make_pop_sender(std::move(state), context);
          });
    }

    template <timed_scheduler_in_context scheduler_context_t,
              typename deadline_t,
              completion_token completion_token_t = use_sender_t>
    [[nodiscard]] auto pop_until(scheduler_context_t context,
                                 const deadline_t &deadline,
                                 completion_token_t token = completion_token_t{})
        -> decltype(auto)
      requires std::is_nothrow_move_constructible_v<value_t>
    {
      return dispatch_token<result<value_t>>(
          std::move(token), [state = state_, context, deadline]() mutable {
            return make_pop_until_sender(std::move(state), context, deadline);
          });
    }

  private:
    friend class channel;
    explicit receiver(std::shared_ptr<state> state)
        : state_(std::move(state)) {}

    std::shared_ptr<state> state_{};
  };

private:
  using queue_type = mpmc_queue<value_t, false, allocator_type>;

  struct state {
    explicit state(const std::size_t capacity, const allocator_type &allocator)
        : queue(capacity, allocator) {}

    queue_type queue;
    sender_notify close_notify;
    std::atomic<std::uint64_t> close_epoch{0U};
    std::atomic<bool> closed{false};
  };

  template <typename result_t> struct close_wait_sender {
    static_assert(detail::result_like<result_t>);

    using sender_concept = stdexec::sender_t;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(result_t), stdexec::set_stopped_t()>;

    template <stdexec::receiver_of<completion_signatures> receiver_t>
    struct operation_state {
      std::shared_ptr<state> shared_state{};
      receiver_t receiver;
      sender_notify::waiter waiter{};
      std::atomic<bool> waiting{false};
      std::atomic<bool> completed{false};
      std::atomic<bool> stop_requested{false};

      struct stop_handler {
        operation_state *self{nullptr};
        void operator()() const noexcept { self->on_stop_requested(); }
      };

      using stop_token_t =
          stdexec::stop_token_of_t<stdexec::env_of_t<receiver_t>>;
      using stop_callback_t =
          stdexec::stop_callback_for_t<stop_token_t, stop_handler>;
      std::optional<stop_callback_t> on_stop{};

      operation_state(std::shared_ptr<state> state_ptr,
                      receiver_t &&receiver_value) noexcept
          : shared_state(std::move(state_ptr)),
            receiver(std::forward<receiver_t>(receiver_value)) {}

      ~operation_state() {
        if (shared_state != nullptr &&
            waiting.exchange(false, std::memory_order_acq_rel)) {
          shared_state->close_notify.disarm(waiter);
        }
      }

      static void notify_waiter(void *owner,
                                sender_notify::waiter *waiter_ptr) noexcept {
        auto *self = static_cast<operation_state *>(owner);
        if (waiter_ptr != &(self->waiter)) {
          return;
        }

        bool expected = true;
        if (!self->waiting.compare_exchange_strong(expected, false,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
          return;
        }

        self->complete_value(result_t::failure(errc::channel_closed));
      }

      void start() & noexcept {
        if constexpr (!stdexec::unstoppable_token<stop_token_t>) {
          auto stop_token = stdexec::get_stop_token(stdexec::get_env(receiver));
          if (stop_token.stop_requested()) {
            complete_stopped();
            return;
          }
          on_stop.emplace(stop_token, stop_handler{this});
          if (stop_token.stop_requested()) {
            on_stop_requested();
            return;
          }
        }

        if (shared_state->closed.load(std::memory_order_acquire)) {
          complete_value(result_t::failure(errc::channel_closed));
          return;
        }

        auto *turn_ptr = &(shared_state->close_epoch);
        const auto current_epoch = turn_ptr->load(std::memory_order_acquire);
        waiter.turn_ptr = turn_ptr;
        waiter.expected_turn = current_epoch + 1U;
        waiter.channel_hint =
            sender_notify::suggest_channel_index(turn_ptr, waiter.expected_turn);
        waiter.channel_index.store(sender_notify::invalid_channel_index,
                                   std::memory_order_relaxed);
        waiter.owner = this;
        waiter.notify = &operation_state::notify_waiter;
        waiter.next = nullptr;
        waiter.prev = nullptr;
        waiting.store(true, std::memory_order_release);

        if (!shared_state->close_notify.arm(waiter)) {
          waiting.store(false, std::memory_order_release);
          complete_value(result_t::failure(errc::channel_closed));
          return;
        }

        if (stop_requested.load(std::memory_order_acquire)) {
          on_stop_requested();
        }
      }

      void complete_value(result_t status) noexcept {
        if (completed.exchange(true, std::memory_order_acq_rel)) {
          return;
        }
        if (waiting.exchange(false, std::memory_order_acq_rel)) {
          shared_state->close_notify.disarm(waiter);
        }
        stdexec::set_value(std::move(receiver), std::move(status));
      }

      void complete_stopped() noexcept {
        if (completed.exchange(true, std::memory_order_acq_rel)) {
          return;
        }
        if (waiting.exchange(false, std::memory_order_acq_rel)) {
          shared_state->close_notify.disarm(waiter);
        }
        stdexec::set_stopped(std::move(receiver));
      }

      void on_stop_requested() noexcept {
        stop_requested.store(true, std::memory_order_release);
        complete_stopped();
      }
    };

    template <stdexec::receiver_of<completion_signatures> receiver_t>
    [[nodiscard]] auto connect(receiver_t receiver) && noexcept(
        std::is_nothrow_move_constructible_v<receiver_t>)
        -> operation_state<receiver_t> {
      return operation_state<receiver_t>{std::move(shared_state_),
                                         std::move(receiver)};
    }

    template <stdexec::receiver_of<completion_signatures> receiver_t>
    [[nodiscard]] auto
    connect(receiver_t receiver) const & -> operation_state<receiver_t>
      requires std::copy_constructible<receiver_t>
    {
      return operation_state<receiver_t>{shared_state_, std::move(receiver)};
    }

    std::shared_ptr<state> shared_state_{};
  };

  template <typename... args_t>
    requires std::constructible_from<value_t, args_t &&...> &&
             std::is_nothrow_constructible_v<value_t, args_t &&...>
  [[nodiscard]] static auto try_push_impl(const std::shared_ptr<state> &state,
                                          args_t &&...args) noexcept
      -> result<void> {
    if (state->closed.load(std::memory_order_acquire)) {
      return result<void>::failure(errc::channel_closed);
    }
    return state->queue.try_push(std::forward<args_t>(args)...);
  }

  [[nodiscard]] static auto try_pop_impl(const std::shared_ptr<state> &state)
      noexcept -> result<value_t>
    requires std::is_nothrow_move_constructible_v<value_t>
  {
    auto value = state->queue.try_pop();
    if (value.has_value()) {
      return value;
    }

    if (value.error() != errc::queue_empty) {
      return value;
    }

    if (state->closed.load(std::memory_order_acquire)) {
      return result<value_t>::failure(errc::channel_closed);
    }

    return value;
  }

  [[nodiscard]] static auto close_impl(const std::shared_ptr<state> &state)
      noexcept -> bool {
    if (state->closed.exchange(true, std::memory_order_acq_rel)) {
      return false;
    }
    auto *turn_ptr = &(state->close_epoch);
    const auto epoch = turn_ptr->fetch_add(1U, std::memory_order_acq_rel) + 1U;
    state->close_notify.notify(turn_ptr, epoch);
    return true;
  }

  template <typename context_t>
  [[nodiscard]] static auto make_push_sender(std::shared_ptr<state> state,
                                             context_t context, value_t value) {
    auto queue_sender = state->queue.push(context, std::move(value), use_sender) |
                        stdexec::then([state](result<void> status) mutable {
                          return status;
                        });
    return exec::when_any(std::move(queue_sender),
                          close_wait_sender<result<void>>{std::move(state)}) |
           stdexec::upon_stopped(
               []() noexcept { return result<void>::failure(errc::canceled); });
  }

  template <typename context_t>
  [[nodiscard]] static auto make_pop_sender(std::shared_ptr<state> state,
                                            context_t context) {
    auto wait_sender =
        exec::when_any(state->queue.pop(context, use_sender),
                       close_wait_sender<result<value_t>>{state}) |
        stdexec::upon_stopped([]() noexcept {
          return result<value_t>::failure(errc::canceled);
        });

    return std::move(wait_sender) |
           stdexec::then([state = std::move(state)](result<value_t> status) {
             if (status.has_value() || status.error() != errc::channel_closed) {
               return status;
             }

             auto drained = state->queue.try_pop();
             if (drained.has_value()) {
               return drained;
             }
             if (drained.error() == errc::queue_empty) {
               return result<value_t>::failure(errc::channel_closed);
             }
             return drained;
           });
  }

  template <typename context_t, typename deadline_t>
  [[nodiscard]] static auto make_push_until_sender(std::shared_ptr<state> state,
                                                   context_t context,
                                                   const deadline_t &deadline,
                                                   value_t value) {
    return timeout_at<result<void>>(
               context,
               make_push_sender(std::move(state), context, std::move(value)),
               deadline) |
           stdexec::upon_error([](auto &&) noexcept {
             return result<void>::failure(errc::unavailable);
           }) |
           stdexec::upon_stopped(
               []() noexcept { return result<void>::failure(errc::canceled); });
  }

  template <typename context_t, typename deadline_t>
  [[nodiscard]] static auto make_pop_until_sender(std::shared_ptr<state> state,
                                                  context_t context,
                                                  const deadline_t &deadline) {
    return timeout_at<result<value_t>>(context,
                                       make_pop_sender(std::move(state), context),
                                       deadline) |
           stdexec::upon_error([](auto &&) noexcept {
             return result<value_t>::failure(errc::unavailable);
           }) |
           stdexec::upon_stopped([]() noexcept {
             return result<value_t>::failure(errc::canceled);
           });
  }

  template <typename result_t, completion_token completion_token_t,
            typename sender_factory_t>
  [[nodiscard]] static auto
  dispatch_token(completion_token_t &&token,
                 sender_factory_t &&sender_factory) -> decltype(auto) {
    using bare_token_t = std::remove_cvref_t<completion_token_t>;

    if constexpr (std::same_as<bare_token_t, use_sender_t>) {
      return std::invoke(std::forward<sender_factory_t>(sender_factory));
    } else if constexpr (std::same_as<bare_token_t, use_awaitable_t>) {
      return detail::make_awaitable_task<result_t>(
          std::invoke(std::forward<sender_factory_t>(sender_factory)));
    } else if constexpr (callback_token<bare_token_t>) {
      auto callback = std::forward<completion_token_t>(token);
      auto handler = std::move(callback.handler);
      const auto stop_token = callback.stop_token;

      if (stop_token.stop_requested()) {
        std::invoke(handler, result_t::failure(errc::canceled));
        return;
      }

      auto sender = std::invoke(std::forward<sender_factory_t>(sender_factory));
      exec::start_detached(
          std::move(sender) |
          stdexec::then([handler = std::move(handler),
                         stop_token](result_t status) mutable {
            if (stop_token.stop_requested() && status.has_value()) {
              status = result_t::failure(errc::canceled);
            }
            std::invoke(handler, std::move(status));
          }));
      return;
    }
  }

  std::shared_ptr<state> state_{};
};

} // namespace wh::core
