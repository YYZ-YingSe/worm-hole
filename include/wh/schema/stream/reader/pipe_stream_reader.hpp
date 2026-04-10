// Defines the reader endpoint of the pipe stream family.
#pragma once

#include <concepts>
#include <memory>
#include <optional>
#include <utility>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/schema/stream/core/status.hpp"
#include "wh/schema/stream/core/stream_base.hpp"
#include "wh/schema/stream/detail/pipe_stream.hpp"

namespace wh::schema::stream {

template <typename value_t>
class pipe_stream_reader final
    : public stream_base<pipe_stream_reader<value_t>, value_t> {
public:
  using state_t = detail::pipe_stream_state<value_t>;
  using chunk_type = stream_chunk<value_t>;

  pipe_stream_reader() = default;
  explicit pipe_stream_reader(const std::shared_ptr<state_t> &state)
      : state_(state) {}
  explicit pipe_stream_reader(std::shared_ptr<state_t> &&state)
      : state_(std::move(state)) {}
  pipe_stream_reader(const pipe_stream_reader &) = delete;
  auto operator=(const pipe_stream_reader &) -> pipe_stream_reader & = delete;
  pipe_stream_reader(pipe_stream_reader &&) noexcept = default;
  auto operator=(pipe_stream_reader &&) noexcept
      -> pipe_stream_reader & = default;

  [[nodiscard]] auto read_impl() -> stream_result<chunk_type> {
    if (!state_) {
      return stream_result<chunk_type>::failure(wh::core::errc::not_found);
    }
    if (state_->reader_closed.load(std::memory_order_acquire)) {
      return stream_result<chunk_type>{chunk_type::make_eof()};
    }

    return map_blocking_pop_to_chunk(state_->queue.pop(), state_);
  }

  [[nodiscard]] auto try_read_impl() -> stream_try_result<chunk_type> {
    if (!state_) {
      return stream_result<chunk_type>::failure(wh::core::errc::not_found);
    }
    if (state_->reader_closed.load(std::memory_order_acquire)) {
      return stream_result<chunk_type>{chunk_type::make_eof()};
    }

    return map_pop_result_to_chunk(
        detail::retry_busy_result([this]() { return state_->queue.try_pop(); }),
        state_);
  }

  [[nodiscard]] auto read_async() const {
    auto async_state = detail::select_pipe_async_state(state_);

    return detail::normalize_pipe_read_sender<value_t>(
        async_state.state->queue.async_pop(), std::move(async_state.state),
        async_state.state_missing, async_state.reader_closed);
  }

  auto close_impl() -> wh::core::result<void> {
    if (!state_) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    state_->reader_closed.store(true, std::memory_order_release);
    state_->queue.close();
    return {};
  }

  [[nodiscard]] auto is_closed_impl() const noexcept -> bool {
    return !state_ || state_->reader_closed.load(std::memory_order_acquire);
  }

  [[nodiscard]] auto is_source_closed() const noexcept -> bool {
    return !state_ || state_->queue.is_closed();
  }

  auto set_automatic_close(const auto_close_options &options) noexcept -> void {
    (void)options;
  }

private:
  [[nodiscard]] static auto
  map_blocking_pop_to_chunk(std::optional<value_t> popped,
                            const std::shared_ptr<state_t> &)
      -> stream_result<chunk_type> {
    if (popped.has_value()) {
      return stream_result<chunk_type>{
          chunk_type::make_value(std::move(*popped))};
    }
    return stream_result<chunk_type>{chunk_type::make_eof()};
  }

  [[nodiscard]] static auto map_pop_result_to_chunk(
      typename wh::core::bounded_queue<value_t>::try_pop_result popped,
      const std::shared_ptr<state_t> &) -> stream_try_result<chunk_type> {
    if (popped.has_value()) {
      return stream_result<chunk_type>{
          chunk_type::make_value(std::move(popped).value())};
    }

    if (popped.error() == wh::core::bounded_queue_status::empty ||
        popped.error() == wh::core::bounded_queue_status::busy ||
        popped.error() == wh::core::bounded_queue_status::busy_async) {
      return stream_pending;
    }

    if (popped.error() == wh::core::bounded_queue_status::closed) {
      return stream_result<chunk_type>{chunk_type::make_eof()};
    }

    return stream_result<chunk_type>::failure(
        detail::map_pipe_queue_status(popped.error()));
  }
  std::shared_ptr<state_t> state_{};
};

} // namespace wh::schema::stream
