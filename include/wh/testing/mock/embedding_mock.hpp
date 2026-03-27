// Defines a scripted embedding mock for deterministic vector generation
// behavior in tests.
#pragma once

#include <concepts>
#include <string>
#include <utility>
#include <vector>

#include "wh/core/result.hpp"
#include "wh/testing/mock/detail/scripted_result_queue.hpp"

namespace wh::testing::mock {

/// Public interface for `embedding_mock`.
class embedding_mock {
public:
  template <typename embedding_t>
    requires std::constructible_from<std::vector<float>, embedding_t &&>
  /// Enqueues a successful scripted result for the next invocation.
  auto enqueue_success(embedding_t &&embedding) -> void {
    scripted_.enqueue_success(std::forward<embedding_t>(embedding));
  }

  /// Enqueues an error scripted result for the next invocation.
  auto enqueue_error(const wh::core::errc code) -> void {
    scripted_.enqueue_error(code);
  }

  /// Generates embeddings for the provided inputs and returns vectors or structured error.
  [[nodiscard]] auto embed([[maybe_unused]] const std::string &text)
      -> wh::core::result<std::vector<float>> {
    ++embed_count_;
    return scripted_.next();
  }

  /// Returns total number of `embed` calls performed.
  [[nodiscard]] auto embed_count() const noexcept -> std::size_t {
    return embed_count_;
  }

private:
  /// Scripted result queue used to drive deterministic mock behavior.
  detail::scripted_result_queue<std::vector<float>> scripted_{};
  /// Total number of `embed` calls.
  std::size_t embed_count_{0U};
};

} // namespace wh::testing::mock
