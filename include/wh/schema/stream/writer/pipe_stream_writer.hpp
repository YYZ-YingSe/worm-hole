// Defines the writer endpoint of the pipe stream family.
#pragma once

#include <memory>
#include <type_traits>
#include <utility>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/schema/stream/detail/pipe_stream.hpp"

namespace wh::schema::stream {

template <typename value_t> class pipe_stream_writer {
public:
  using value_type = value_t;
  using state_t = detail::pipe_stream_state<value_t>;
  using chunk_type = stream_chunk<value_t>;

  pipe_stream_writer() = default;
  explicit pipe_stream_writer(const std::shared_ptr<state_t> &state)
      : state_(state) {}
  explicit pipe_stream_writer(std::shared_ptr<state_t> &&state)
      : state_(std::move(state)) {}
  pipe_stream_writer(const pipe_stream_writer &) = delete;
  auto operator=(const pipe_stream_writer &) -> pipe_stream_writer & = delete;
  pipe_stream_writer(pipe_stream_writer &&) noexcept = default;
  auto operator=(pipe_stream_writer &&) noexcept
      -> pipe_stream_writer & = default;

  auto try_write(const value_t &value) -> wh::core::result<void>
    requires std::is_nothrow_copy_constructible_v<value_t>
  {
    if (!state_) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    if (state_->reader_closed.load(std::memory_order_acquire)) {
      return wh::core::result<void>::failure(wh::core::errc::channel_closed);
    }

    const auto status = detail::retry_busy_status(
        [this, &value]() { return state_->queue.try_push(value); });
    if (status == wh::core::bounded_queue_status::success) {
      return {};
    }
    return wh::core::result<void>::failure(
        detail::map_pipe_queue_status(status));
  }

  auto try_write(value_t &&value) -> wh::core::result<void>
    requires std::is_nothrow_move_constructible_v<value_t>
  {
    if (!state_) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    if (state_->reader_closed.load(std::memory_order_acquire)) {
      return wh::core::result<void>::failure(wh::core::errc::channel_closed);
    }

    const auto status = detail::retry_busy_status(
        [this, &value]() { return state_->queue.try_push(std::move(value)); });
    if (status == wh::core::bounded_queue_status::success) {
      return {};
    }
    return wh::core::result<void>::failure(
        detail::map_pipe_queue_status(status));
  }

  [[nodiscard]] auto write_async(const value_t &value) const
    requires std::is_nothrow_copy_constructible_v<value_t>
  {
    const bool state_missing = !state_;
    const bool reader_closed =
        !state_missing && state_->reader_closed.load(std::memory_order_acquire);
    auto target_state = (state_missing || reader_closed)
                            ? detail::shared_closed_pipe_state<state_t>()
                            : state_;

    return detail::normalize_pipe_write_sender(
        target_state->queue.async_push(value), std::move(target_state),
        state_missing, reader_closed);
  }

  [[nodiscard]] auto write_async(value_t &&value) const
    requires std::is_nothrow_move_constructible_v<value_t>
  {
    const bool state_missing = !state_;
    const bool reader_closed =
        !state_missing && state_->reader_closed.load(std::memory_order_acquire);
    auto target_state = (state_missing || reader_closed)
                            ? detail::shared_closed_pipe_state<state_t>()
                            : state_;

    return detail::normalize_pipe_write_sender(
        target_state->queue.async_push(std::move(value)),
        std::move(target_state), state_missing, reader_closed);
  }

  auto close() -> wh::core::result<void> {
    if (!state_) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    state_->queue.close();
    return {};
  }

  [[nodiscard]] auto is_closed() const noexcept -> bool {
    return !state_ || state_->queue.is_closed();
  }

private:
  std::shared_ptr<state_t> state_{};
};

} // namespace wh::schema::stream
