// Defines a generic scripted result queue used by mocks to return
// deterministic success/error sequences.
#pragma once

#include <deque>
#include <utility>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"

namespace wh::testing::helper::mock::detail {

template <typename value_t> class scripted_result_queue {
public:
  using result_t = wh::core::result<value_t>;

  auto enqueue_success(value_t value) -> void {
    scripted_results_.push_back(result_t{std::move(value)});
  }

  auto enqueue_error(const wh::core::errc code) -> void {
    scripted_results_.push_back(result_t::failure(code));
  }

  auto set_default_error(const wh::core::errc code) -> void { default_error_ = code; }

  [[nodiscard]] auto next() -> result_t {
    if (scripted_results_.empty()) {
      return result_t::failure(default_error_);
    }

    result_t next_result = std::move(scripted_results_.front());
    scripted_results_.pop_front();
    return next_result;
  }

  [[nodiscard]] auto pending() const noexcept -> std::size_t { return scripted_results_.size(); }

  auto clear() -> void { scripted_results_.clear(); }

private:
  std::deque<result_t> scripted_results_{};
  wh::core::errc default_error_{wh::core::errc::not_found};
};

template <> class scripted_result_queue<void> {
public:
  using result_t = wh::core::result<void>;

  auto enqueue_success() -> void { scripted_results_.push_back(result_t{}); }

  auto enqueue_error(const wh::core::errc code) -> void {
    scripted_results_.push_back(result_t::failure(code));
  }

  auto set_default_error(const wh::core::errc code) -> void { default_error_ = code; }

  [[nodiscard]] auto next() -> result_t {
    if (scripted_results_.empty()) {
      return result_t::failure(default_error_);
    }

    result_t next_result = std::move(scripted_results_.front());
    scripted_results_.pop_front();
    return next_result;
  }

  [[nodiscard]] auto pending() const noexcept -> std::size_t { return scripted_results_.size(); }

  auto clear() -> void { scripted_results_.clear(); }

private:
  std::deque<result_t> scripted_results_{};
  wh::core::errc default_error_{wh::core::errc::not_found};
};

} // namespace wh::testing::helper::mock::detail
