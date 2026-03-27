// Provides declarations and utilities for `wh/testing/mock/retriever_mock.hpp`.
#pragma once

#include <concepts>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "wh/core/result.hpp"
#include "wh/testing/mock/detail/scripted_result_queue.hpp"

namespace wh::testing::mock {

/// Public interface for `retriever_mock`.
class retriever_mock {
public:
  template <typename documents_t>
    requires std::constructible_from<std::vector<std::string>, documents_t &&>
  /// Enqueues a successful scripted result for the next invocation.
  auto enqueue_success(documents_t &&documents) -> void {
    scripted_.enqueue_success(std::forward<documents_t>(documents));
  }

  /// Enqueues a successful scripted result for the next invocation.
  auto enqueue_success(std::initializer_list<std::string> documents) -> void {
    scripted_.enqueue_success(std::vector<std::string>{documents});
  }

  /// Enqueues an error scripted result for the next invocation.
  auto enqueue_error(const wh::core::errc code) -> void {
    scripted_.enqueue_error(code);
  }

  /// Retrieves matching documents for the request query/options and returns ranked results.
  [[nodiscard]] auto retrieve([[maybe_unused]] const std::string &query)
      -> wh::core::result<std::vector<std::string>> {
    ++retrieve_count_;
    return scripted_.next();
  }

  /// Returns total number of `retrieve` calls performed.
  [[nodiscard]] auto retrieve_count() const noexcept -> std::size_t {
    return retrieve_count_;
  }

private:
  /// Scripted result queue used to drive deterministic mock behavior.
  detail::scripted_result_queue<std::vector<std::string>> scripted_{};
  /// Total number of `retrieve` calls.
  std::size_t retrieve_count_{0U};
};

} // namespace wh::testing::mock
