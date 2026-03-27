// Provides declarations and utilities for `wh/testing/mock/stream_mock.hpp`.
#pragma once

#include <concepts>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/testing/mock/detail/scripted_result_queue.hpp"

namespace wh::testing::mock {

/// Public interface for `stream_mock`.
class stream_mock {
public:
  /// Behavior returned after scripted queue is exhausted.
  enum class eof_behavior : std::uint8_t {
    /// Emit EOF once pending chunks are consumed.
    close = 0U,
    /// Return configured EOF error result.
    error,
    /// Repeat last chunk instead of closing stream at EOF.
    repeat_last,
  };

  template <typename chunk_t>
    requires std::constructible_from<std::string, chunk_t &&>
  /// Enqueues one stream chunk that will be returned by subsequent `read` calls.
  auto enqueue_chunk(chunk_t &&chunk) -> void {
    scripted_.enqueue_success(std::forward<chunk_t>(chunk));
  }

  /// Enqueues an error scripted result for the next invocation.
  auto enqueue_error(const wh::core::errc code) -> void {
    scripted_.enqueue_error(code);
  }

  /// Configures end-of-stream behavior for the scripted stream mock.
  auto set_eof_behavior(
      const eof_behavior behavior,
      const wh::core::errc eof_error = wh::core::errc::channel_closed) -> void {
    eof_behavior_ = behavior;
    eof_error_ = eof_error;
  }

  /// Pops and returns the next scripted result entry from the queue.
  [[nodiscard]] auto read() -> wh::core::result<std::string> {
    auto next_result = scripted_.next();
    if (next_result.has_value()) {
      last_chunk_ = next_result.value();
      return next_result;
    }

    if (next_result.error() != wh::core::errc::not_found) {
      return next_result;
    }

    if (eof_behavior_ == eof_behavior::repeat_last && last_chunk_.has_value()) {
      return *last_chunk_;
    }
    if (eof_behavior_ == eof_behavior::error) {
      return wh::core::result<std::string>::failure(eof_error_);
    }
    return wh::core::result<std::string>::failure(
        wh::core::errc::channel_closed);
  }

  /// Returns number of scripted entries pending in the queue.
  [[nodiscard]] auto pending() const noexcept -> std::size_t {
    return scripted_.pending();
  }

private:
  /// Scripted chunk/error queue used by `read`.
  detail::scripted_result_queue<std::string> scripted_{};
  /// Behavior applied after scripted queue is exhausted.
  eof_behavior eof_behavior_{eof_behavior::close};
  /// Error code returned when `eof_behavior_ == error`.
  wh::core::errc eof_error_{wh::core::errc::channel_closed};
  /// Last chunk cache used by `repeat_last` EOF mode.
  std::optional<std::string> last_chunk_{};
};

} // namespace wh::testing::mock
