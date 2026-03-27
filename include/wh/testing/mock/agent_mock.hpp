// Defines a deterministic mock agent used by tests to script responses
// and verify call counts/inputs.
#pragma once

#include <concepts>
#include <string>
#include <utility>

#include "wh/core/result.hpp"
#include "wh/testing/mock/detail/scripted_result_queue.hpp"

namespace wh::testing::mock {

/// Public interface for `agent_mock`.
class agent_mock {
public:
  template <typename output_t>
    requires std::constructible_from<std::string, output_t &&>
  /// Enqueues a successful scripted result for the next invocation.
  auto enqueue_success(output_t &&output) -> void {
    scripted_.enqueue_success(std::forward<output_t>(output));
  }

  /// Enqueues an error scripted result for the next invocation.
  auto enqueue_error(const wh::core::errc code) -> void {
    scripted_.enqueue_error(code);
  }

  /// Enqueues an interrupt scripted result for the next invocation.
  auto enqueue_interrupt() -> void {
    scripted_.enqueue_error(wh::core::errc::canceled);
  }

  /// Executes one scripted agent run and returns the next queued result.
  [[nodiscard]] auto run([[maybe_unused]] const std::string &input)
      -> wh::core::result<std::string> {
    ++run_count_;
    return scripted_.next();
  }

  /// Returns total number of `run` calls performed.
  [[nodiscard]] auto run_count() const noexcept -> std::size_t {
    return run_count_;
  }

private:
  /// Scripted result queue used to drive deterministic mock behavior.
  detail::scripted_result_queue<std::string> scripted_{};
  /// Total number of `run` calls.
  std::size_t run_count_{0U};
};

} // namespace wh::testing::mock
