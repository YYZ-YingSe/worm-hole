#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/testing/mock/detail/scripted_result_queue.hpp"

namespace wh::testing::mock {

class stream_mock {
public:
  enum class eof_behavior : std::uint8_t {
    close = 0U,
    error,
    repeat_last,
  };

  auto enqueue_chunk(std::string chunk) -> void {
    scripted_.enqueue_success(std::move(chunk));
  }

  auto enqueue_error(const wh::core::errc code) -> void {
    scripted_.enqueue_error(code);
  }

  auto set_eof_behavior(
      const eof_behavior behavior,
      const wh::core::errc eof_error = wh::core::errc::channel_closed) -> void {
    eof_behavior_ = behavior;
    eof_error_ = eof_error;
  }

  [[nodiscard]] auto next() -> wh::core::result<std::string> {
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

  [[nodiscard]] auto pending() const noexcept -> std::size_t {
    return scripted_.pending();
  }

private:
  detail::scripted_result_queue<std::string> scripted_{};
  eof_behavior eof_behavior_{eof_behavior::close};
  wh::core::errc eof_error_{wh::core::errc::channel_closed};
  std::optional<std::string> last_chunk_{};
};

} // namespace wh::testing::mock
