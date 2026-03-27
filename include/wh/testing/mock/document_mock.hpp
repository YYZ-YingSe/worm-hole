// Defines a deterministic document mock used to script document operation
// outputs in tests.
#pragma once

#include <concepts>
#include <string>
#include <utility>

#include "wh/core/result.hpp"
#include "wh/testing/mock/detail/scripted_result_queue.hpp"

namespace wh::testing::mock {

/// Public interface for `document_mock`.
class document_mock {
public:
  template <typename content_t>
    requires std::constructible_from<std::string, content_t &&>
  /// Enqueues a successful scripted result for the next invocation.
  auto enqueue_success(content_t &&content) -> void {
    scripted_.enqueue_success(std::forward<content_t>(content));
  }

  /// Enqueues an error scripted result for the next invocation.
  auto enqueue_error(const wh::core::errc code) -> void {
    scripted_.enqueue_error(code);
  }

  /// Executes one scripted load/fetch operation and returns the next queued result.
  [[nodiscard]] auto load([[maybe_unused]] const std::string &document_id)
      -> wh::core::result<std::string> {
    ++load_count_;
    return scripted_.next();
  }

  /// Returns total number of load calls performed.
  [[nodiscard]] auto load_count() const noexcept -> std::size_t {
    return load_count_;
  }

private:
  /// Scripted result queue used to drive deterministic mock behavior.
  detail::scripted_result_queue<std::string> scripted_{};
  /// Total number of `load` calls.
  std::size_t load_count_{0U};
};

} // namespace wh::testing::mock
