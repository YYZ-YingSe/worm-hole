// Defines shared internal state and helpers for the pipe stream family.
#pragma once

#include <atomic>
#include <concepts>
#include <cstddef>
#include <exception>
#include <memory>
#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

#include "wh/core/bounded_queue.hpp"
#include "wh/core/compiler.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/schema/stream/core/status.hpp"
#include "wh/schema/stream/core/types.hpp"

namespace wh::schema::stream::detail {

template <typename state_t>
inline auto
keep_pipe_state_alive(const std::shared_ptr<state_t> &state) noexcept -> void {
  [[maybe_unused]] const auto *raw_state = state.get();
}

template <typename state_t>
[[nodiscard]] inline auto shared_closed_pipe_state()
    -> const std::shared_ptr<state_t> & {
  static const auto closed_state = [] {
    auto state = std::make_shared<state_t>(1U);
    state->queue.close();
    return state;
  }();
  return closed_state;
}

[[nodiscard]] inline auto
map_pipe_queue_status(const wh::core::bounded_queue_status status) noexcept
    -> wh::core::error_code {
  using status_t = wh::core::bounded_queue_status;

  switch (status) {
  case status_t::success:
    return wh::core::errc::ok;
  case status_t::empty:
    return wh::core::errc::queue_empty;
  case status_t::full:
    return wh::core::errc::queue_full;
  case status_t::closed:
    return wh::core::errc::channel_closed;
  case status_t::busy:
  case status_t::busy_async:
    return wh::core::errc::unavailable;
  }

  return wh::core::errc::unavailable;
}

template <typename attempt_t>
[[nodiscard]] inline auto retry_busy_status(attempt_t &&attempt) noexcept(
    noexcept(std::forward<attempt_t>(attempt)()))
    -> wh::core::bounded_queue_status {
  auto attempt_fn = std::forward<attempt_t>(attempt);
  auto status = attempt_fn();
  while (status == wh::core::bounded_queue_status::busy) {
    wh::core::spin_pause();
    status = attempt_fn();
  }
  return status;
}

template <typename attempt_t>
[[nodiscard]] inline auto retry_busy_result(attempt_t &&attempt) noexcept(
    noexcept(std::forward<attempt_t>(attempt)())) {
  auto attempt_fn = std::forward<attempt_t>(attempt);
  auto result = attempt_fn();
  while (result.has_error() &&
         result.error() == wh::core::bounded_queue_status::busy) {
    wh::core::spin_pause();
    result = attempt_fn();
  }
  return result;
}

template <typename result_t>
[[nodiscard]] inline auto rethrow_pipe_exception(std::exception_ptr error)
    -> result_t {
  std::rethrow_exception(std::move(error));
}

template <typename state_t, stdexec::sender sender_t>
[[nodiscard]] inline auto
normalize_pipe_write_sender(sender_t &&sender, std::shared_ptr<state_t> state,
                            const bool state_missing,
                            const bool reader_closed) {
  auto success_state = state;
  auto error_state = std::move(state);
  return stdexec::upon_error(
      stdexec::then(std::forward<sender_t>(sender),
                    [state = std::move(success_state), state_missing,
                     reader_closed]() mutable -> wh::core::result<void> {
                      keep_pipe_state_alive(state);
                      if (state_missing) {
                        return wh::core::result<void>::failure(
                            wh::core::errc::not_found);
                      }
                      if (reader_closed) {
                        return wh::core::result<void>::failure(
                            wh::core::errc::channel_closed);
                      }
                      return {};
                    }),
      [state = std::move(error_state), state_missing,
       reader_closed](auto error) mutable -> wh::core::result<void> {
        keep_pipe_state_alive(state);
        using error_t = std::remove_cvref_t<decltype(error)>;
        if constexpr (std::same_as<error_t, wh::core::bounded_queue_status>) {
          if (state_missing) {
            return wh::core::result<void>::failure(wh::core::errc::not_found);
          }
          if (reader_closed ||
              error == wh::core::bounded_queue_status::closed) {
            return wh::core::result<void>::failure(
                wh::core::errc::channel_closed);
          }
          return wh::core::result<void>::failure(map_pipe_queue_status(error));
        } else {
          return rethrow_pipe_exception<wh::core::result<void>>(
              std::forward<decltype(error)>(error));
        }
      });
}

template <typename value_t, typename state_t, stdexec::sender sender_t>
[[nodiscard]] inline auto
normalize_pipe_read_sender(sender_t &&sender, std::shared_ptr<state_t> state,
                           const bool state_missing, const bool reader_closed) {
  using chunk_type = stream_chunk<value_t>;
  using result_t = stream_result<chunk_type>;
  auto success_state = state;
  auto error_state = std::move(state);

  return stdexec::upon_error(
      stdexec::then(std::forward<sender_t>(sender),
                    [state = std::move(success_state), state_missing,
                     reader_closed](value_t value) mutable -> result_t {
                      keep_pipe_state_alive(state);
                      if (state_missing) {
                        return result_t::failure(wh::core::errc::not_found);
                      }
                      if (reader_closed) {
                        return result_t{chunk_type::make_eof()};
                      }
                      return result_t{chunk_type::make_value(std::move(value))};
                    }),
      [state = std::move(error_state), state_missing,
       reader_closed](auto error) mutable -> result_t {
        using error_t = std::remove_cvref_t<decltype(error)>;
        if constexpr (std::same_as<error_t, wh::core::bounded_queue_status>) {
          if (state_missing) {
            return result_t::failure(wh::core::errc::not_found);
          }
          if (reader_closed ||
              error == wh::core::bounded_queue_status::closed) {
            return result_t{chunk_type::make_eof()};
          }
          return result_t::failure(map_pipe_queue_status(error));
        } else {
          return rethrow_pipe_exception<result_t>(
              std::forward<decltype(error)>(error));
        }
      });
}

template <typename value_t> struct pipe_stream_state {
  explicit pipe_stream_state(const std::size_t capacity)
      : queue(capacity == 0U ? 1U : capacity) {}

  wh::core::bounded_queue<value_t> queue;
  std::atomic<bool> reader_closed{false};
  std::atomic<bool> eof_emitted{false};
};

} // namespace wh::schema::stream::detail
