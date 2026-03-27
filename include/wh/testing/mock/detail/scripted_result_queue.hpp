// Defines a generic scripted result queue used by mocks to return
// deterministic success/error sequences.
#pragma once

#include <deque>
#include <utility>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"

namespace wh::testing::mock::detail {

template <typename value_t> class scripted_result_queue {
public:
  using result_t = wh::core::result<value_t>;

  scripted_result_queue() = default;

  /// Enqueues a successful scripted result for the next invocation.
  auto enqueue_success(value_t value) -> void {
    scripted_results_.push_back(result_t{std::move(value)});
  }

  /// Enqueues an error scripted result for the next invocation.
  auto enqueue_error(const wh::core::errc code) -> void {
    scripted_results_.push_back(result_t::failure(code));
  }

  /// Sets fallback error returned when scripted queue is empty.
  auto set_default_error(const wh::core::errc code) -> void {
    default_error_ = code;
  }

  /// Pops and returns the next scripted result entry from the queue.
  [[nodiscard]] auto next() -> result_t {
    if (scripted_results_.empty()) {
      return result_t::failure(default_error_);
    }

    result_t next_result = std::move(scripted_results_.front());
    scripted_results_.pop_front();
    return next_result;
  }

  /// Returns number of scripted entries pending in the queue.
  [[nodiscard]] auto pending() const noexcept -> std::size_t {
    return scripted_results_.size();
  }

  /// Clears all queued scripted results and resets queue state.
  auto clear() -> void { scripted_results_.clear(); }

private:
  /// FIFO queue of scripted results consumed by test doubles.
  std::deque<result_t> scripted_results_{};
  /// Error returned when queue is empty.
  wh::core::errc default_error_{wh::core::errc::not_found};
};

template <> class scripted_result_queue<void> {
public:
  using result_t = wh::core::result<void>;

  /// Enqueues a successful scripted result for the next invocation.
  auto enqueue_success() -> void { scripted_results_.push_back(result_t{}); }

  /// Enqueues an error scripted result for the next invocation.
  auto enqueue_error(const wh::core::errc code) -> void {
    scripted_results_.push_back(result_t::failure(code));
  }

  /// Sets fallback error returned when scripted queue is empty.
  auto set_default_error(const wh::core::errc code) -> void {
    default_error_ = code;
  }

  /// Pops and returns the next scripted result entry from the queue.
  [[nodiscard]] auto next() -> result_t {
    if (scripted_results_.empty()) {
      return result_t::failure(default_error_);
    }

    result_t next_result = std::move(scripted_results_.front());
    scripted_results_.pop_front();
    return next_result;
  }

  /// Returns number of scripted entries pending in the queue.
  [[nodiscard]] auto pending() const noexcept -> std::size_t {
    return scripted_results_.size();
  }

  /// Clears all queued scripted results and resets queue state.
  auto clear() -> void { scripted_results_.clear(); }

private:
  /// FIFO queue of scripted results consumed by test doubles.
  std::deque<result_t> scripted_results_{};
  /// Error returned when queue is empty.
  wh::core::errc default_error_{wh::core::errc::not_found};
};

} // namespace wh::testing::mock::detail
