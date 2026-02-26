#pragma once

#include <atomic>
#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#include <exec/start_detached.hpp>
#include <stdexec/execution.hpp>

#include "wh/async/async_initiate.hpp"
#include "wh/async/completion_token_helper.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/scheduler/scheduler_context.hpp"
#include "wh/schema/stream/stream_base.hpp"
#include "wh/sync/channel.hpp"

namespace wh::schema::stream {

namespace detail {

template <typename result_t, wh::core::completion_token completion_token_t,
          typename sender_factory_t>
[[nodiscard]] inline auto
dispatch_stream_token(completion_token_t &&token,
                      sender_factory_t &&sender_factory) -> decltype(auto) {
  using bare_token_t = std::remove_cvref_t<completion_token_t>;

  if constexpr (std::same_as<bare_token_t, wh::core::use_sender_t>) {
    return std::invoke(std::forward<sender_factory_t>(sender_factory));
  } else if constexpr (std::same_as<bare_token_t, wh::core::use_awaitable_t>) {
    return wh::core::detail::make_awaitable_task<result_t>(
        std::invoke(std::forward<sender_factory_t>(sender_factory)));
  } else if constexpr (wh::core::callback_token<bare_token_t>) {
    auto callback = std::forward<completion_token_t>(token);
    auto handler = std::move(callback.handler);
    const auto stop_token = callback.stop_token;

    if (stop_token.stop_requested()) {
      std::invoke(handler, result_t::failure(wh::core::errc::canceled));
      return;
    }

    auto sender = std::invoke(std::forward<sender_factory_t>(sender_factory));
    exec::start_detached(
        std::move(sender) |
        stdexec::then([handler = std::move(handler),
                       stop_token](result_t status) mutable {
          if (stop_token.stop_requested() && status.has_value()) {
            status = result_t::failure(wh::core::errc::canceled);
          }
          std::invoke(handler, std::move(status));
        }));
    return;
  }
}

template <typename state_t>
[[nodiscard]] inline auto shared_closed_stream_state()
    -> const std::shared_ptr<state_t> & {
  static const auto closed_state = [] {
    auto state = std::make_shared<state_t>(1U);
    static_cast<void>(state->queue.close());
    return state;
  }();
  return closed_state;
}

} // namespace detail

template <typename value_t> class pipe_stream_reader;
template <typename value_t> class pipe_stream_writer;

template <typename value_t> struct pipe_stream_state {
  explicit pipe_stream_state(const std::size_t capacity)
      : queue(wh::core::channel_options{capacity == 0U ? 1U : capacity}) {}

  wh::core::channel<value_t> queue;
  std::atomic<bool> reader_closed{false};
  std::atomic<bool> eof_emitted{false};
};

template <typename value_t>
class pipe_stream_reader final
    : public stream_base<pipe_stream_reader<value_t>, value_t> {
public:
  using state_t = pipe_stream_state<value_t>;
  using chunk_type = stream_chunk<value_t>;

  pipe_stream_reader() = default;
  explicit pipe_stream_reader(std::shared_ptr<state_t> state)
      : state_(std::move(state)) {}

  [[nodiscard]] auto next_impl() -> wh::core::result<chunk_type> {
    if (!state_) {
      return wh::core::result<chunk_type>::failure(wh::core::errc::not_found);
    }
    if (state_->reader_closed.load(std::memory_order_acquire)) {
      return wh::core::result<chunk_type>::failure(
          wh::core::errc::channel_closed);
    }

    return pipe_stream_reader::map_pop_result_to_chunk(
        std::move(state_->queue.try_pop()), state_);
  }

  template <
      wh::core::scheduler_context_like scheduler_context_t,
      wh::core::completion_token completion_token_t = wh::core::use_sender_t>
  [[nodiscard]] auto recv(scheduler_context_t context,
                          completion_token_t token = completion_token_t{})
      -> decltype(auto)
    requires std::is_nothrow_move_constructible_v<value_t>
  {
    const bool state_missing = !state_;
    const bool reader_closed =
        !state_missing && state_->reader_closed.load(std::memory_order_acquire);
    auto target_state =
        (state_missing || reader_closed)
            ? detail::shared_closed_stream_state<state_t>()
            : state_;

    return detail::dispatch_stream_token<wh::core::result<chunk_type>>(
        std::move(token),
        [state = std::move(target_state), context, state_missing,
         reader_closed]() {
          return state->queue.pop(context, wh::core::use_sender) |
                 stdexec::then(
                     [state, state_missing, reader_closed](
                         wh::core::result<value_t> popped) {
                       if (state_missing) {
                         return wh::core::result<chunk_type>::failure(
                             wh::core::errc::not_found);
                       }
                       if (reader_closed) {
                         return wh::core::result<chunk_type>::failure(
                             wh::core::errc::channel_closed);
                       }
                       return pipe_stream_reader::map_pop_result_to_chunk(
                           std::move(popped), state);
                     });
        });
  }

  auto close_impl() -> wh::core::result<void> {
    if (!state_) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    state_->reader_closed.store(true, std::memory_order_release);
    static_cast<void>(state_->queue.close());
    return {};
  }

  [[nodiscard]] auto is_closed_impl() const noexcept -> bool {
    return !state_ || state_->reader_closed.load(std::memory_order_acquire);
  }

  auto set_automatic_close(const auto_close_options options) noexcept -> void {
    automatic_close_ = options.enabled;
  }

private:
  [[nodiscard]] static auto
  map_pop_result_to_chunk(wh::core::result<value_t> popped,
                          const std::shared_ptr<state_t> &state)
      -> wh::core::result<chunk_type> {
    if (popped.has_value()) {
      return chunk_type::make_value(std::move(popped).value());
    }
    if (popped.error() == wh::core::errc::queue_empty) {
      return wh::core::result<chunk_type>::failure(wh::core::errc::queue_empty);
    }
    if (popped.error() == wh::core::errc::channel_closed) {
      bool expected = false;
      if (!state->eof_emitted.compare_exchange_strong(
              expected, true, std::memory_order_acq_rel)) {
        return wh::core::result<chunk_type>::failure(
            wh::core::errc::channel_closed);
      }
      return chunk_type::make_eof();
    }
    return wh::core::result<chunk_type>::failure(popped.error());
  }

  std::shared_ptr<state_t> state_{};
  bool automatic_close_{true};
};

template <typename value_t> class pipe_stream_writer {
public:
  using state_t = pipe_stream_state<value_t>;
  using chunk_type = stream_chunk<value_t>;

  pipe_stream_writer() = default;
  explicit pipe_stream_writer(std::shared_ptr<state_t> state)
      : state_(std::move(state)) {}

  auto try_write(value_t value) -> wh::core::result<void>
    requires std::is_nothrow_move_constructible_v<value_t>
  {
    if (!state_) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    if (state_->reader_closed.load(std::memory_order_acquire)) {
      return wh::core::result<void>::failure(wh::core::errc::channel_closed);
    }
    return state_->queue.try_push(std::move(value));
  }

  template <
      wh::core::scheduler_context_like scheduler_context_t,
      wh::core::completion_token completion_token_t = wh::core::use_sender_t>
  [[nodiscard]] auto
  write_async(scheduler_context_t context, const value_t &value,
              completion_token_t token = completion_token_t{}) -> decltype(auto)
    requires std::is_nothrow_copy_constructible_v<value_t>
  {
    const bool state_missing = !state_;
    const bool reader_closed =
        !state_missing && state_->reader_closed.load(std::memory_order_acquire);
    auto target_state =
        (state_missing || reader_closed)
            ? detail::shared_closed_stream_state<state_t>()
            : state_;

    return detail::dispatch_stream_token<wh::core::result<void>>(
        std::move(token),
        [state = std::move(target_state), context, copied = value, state_missing,
         reader_closed]() mutable {
          return state->queue.push(context, std::move(copied),
                                   wh::core::use_sender) |
                 stdexec::then([state_missing,
                                reader_closed](wh::core::result<void> status) {
                   if (state_missing) {
                     return wh::core::result<void>::failure(
                         wh::core::errc::not_found);
                   }
                   if (reader_closed) {
                     return wh::core::result<void>::failure(
                         wh::core::errc::channel_closed);
                   }
                   return status;
                 });
        });
  }

  template <
      wh::core::scheduler_context_like scheduler_context_t,
      wh::core::completion_token completion_token_t = wh::core::use_sender_t>
  [[nodiscard]] auto
  write_async(scheduler_context_t context, value_t &&value,
              completion_token_t token = completion_token_t{}) -> decltype(auto)
    requires std::is_nothrow_move_constructible_v<value_t>
  {
    const bool state_missing = !state_;
    const bool reader_closed =
        !state_missing && state_->reader_closed.load(std::memory_order_acquire);
    auto target_state =
        (state_missing || reader_closed)
            ? detail::shared_closed_stream_state<state_t>()
            : state_;

    return detail::dispatch_stream_token<wh::core::result<void>>(
        std::move(token),
        [state = std::move(target_state), context, moved = std::move(value),
         state_missing, reader_closed]() mutable {
          return state->queue.push(context, std::move(moved),
                                   wh::core::use_sender) |
                 stdexec::then([state_missing,
                                reader_closed](wh::core::result<void> status) {
                   if (state_missing) {
                     return wh::core::result<void>::failure(
                         wh::core::errc::not_found);
                   }
                   if (reader_closed) {
                     return wh::core::result<void>::failure(
                         wh::core::errc::channel_closed);
                   }
                   return status;
                 });
        });
  }

  auto close() -> wh::core::result<void> {
    if (!state_) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    static_cast<void>(state_->queue.close());
    return {};
  }

  [[nodiscard]] auto is_closed() const noexcept -> bool {
    return !state_ || state_->queue.is_closed();
  }

private:
  std::shared_ptr<state_t> state_{};
};

template <typename value_t>
[[nodiscard]] inline auto make_pipe_stream(const std::size_t capacity = 64U)
    -> std::pair<pipe_stream_writer<value_t>, pipe_stream_reader<value_t>> {
  auto state = std::make_shared<pipe_stream_state<value_t>>(capacity);
  return {pipe_stream_writer<value_t>{state},
          pipe_stream_reader<value_t>{state}};
}

} // namespace wh::schema::stream
