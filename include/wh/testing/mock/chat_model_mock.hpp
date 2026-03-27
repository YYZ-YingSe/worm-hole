// Defines a scripted chat model mock that queues success/error outcomes
// for deterministic chat model testing.
#pragma once

#include <concepts>
#include <string>
#include <utility>

#include "wh/core/result.hpp"
#include "wh/testing/mock/detail/scripted_result_queue.hpp"

namespace wh::testing::mock {

/// Public interface for `chat_model_mock`.
class chat_model_mock {
public:
  template <typename completion_t>
    requires std::constructible_from<std::string, completion_t &&>
  /// Enqueues a successful scripted result for the next invocation.
  auto enqueue_success(completion_t &&completion) -> void {
    scripted_.enqueue_success(std::forward<completion_t>(completion));
  }

  /// Enqueues an error scripted result for the next invocation.
  auto enqueue_error(const wh::core::errc code) -> void {
    scripted_.enqueue_error(code);
  }

  /// Enqueues an interrupt scripted result for the next invocation.
  auto enqueue_interrupt() -> void {
    scripted_.enqueue_error(wh::core::errc::canceled);
  }

  /// Executes one scripted completion call and returns the next queued result.
  [[nodiscard]] auto complete([[maybe_unused]] const std::string &prompt)
      -> wh::core::result<std::string> {
    ++complete_count_;
    return scripted_.next();
  }

  /// Returns total number of completion calls performed.
  [[nodiscard]] auto complete_count() const noexcept -> std::size_t {
    return complete_count_;
  }

private:
  /// Scripted result queue used to drive deterministic mock behavior.
  detail::scripted_result_queue<std::string> scripted_{};
  /// Total number of `complete` calls.
  std::size_t complete_count_{0U};
};

} // namespace wh::testing::mock
