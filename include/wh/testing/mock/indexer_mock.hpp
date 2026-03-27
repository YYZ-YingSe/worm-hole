// Provides declarations and utilities for `wh/testing/mock/indexer_mock.hpp`.
#pragma once

#include <string>
#include <vector>

#include "wh/core/result.hpp"
#include "wh/testing/mock/detail/scripted_result_queue.hpp"

namespace wh::testing::mock {

/// Public interface for `indexer_mock`.
class indexer_mock {
public:
  /// Enqueues a successful scripted result for the next invocation.
  auto enqueue_success() -> void { scripted_.enqueue_success(); }

  /// Enqueues an error scripted result for the next invocation.
  auto enqueue_error(const wh::core::errc code) -> void {
    scripted_.enqueue_error(code);
  }

  /// Executes one scripted upsert/write operation and returns the next queued result.
  [[nodiscard]] auto
  upsert([[maybe_unused]] const std::string &document_id,
         [[maybe_unused]] const std::vector<float> &embedding)
      -> wh::core::result<void> {
    ++upsert_count_;
    return scripted_.next();
  }

  /// Returns total number of upsert calls performed.
  [[nodiscard]] auto upsert_count() const noexcept -> std::size_t {
    return upsert_count_;
  }

private:
  /// Scripted result queue used to drive deterministic mock behavior.
  detail::scripted_result_queue<void> scripted_{};
  /// Total number of `upsert` calls.
  std::size_t upsert_count_{0U};
};

} // namespace wh::testing::mock
