#pragma once

#include <string>
#include <vector>

#include "wh/core/result.hpp"
#include "wh/testing/mock/detail/scripted_result_queue.hpp"

namespace wh::testing::mock {

class retriever_mock {
public:
  auto enqueue_success(std::vector<std::string> documents) -> void {
    scripted_.enqueue_success(std::move(documents));
  }

  auto enqueue_error(const wh::core::errc code) -> void {
    scripted_.enqueue_error(code);
  }

  [[nodiscard]] auto retrieve([[maybe_unused]] const std::string &query)
      -> wh::core::result<std::vector<std::string>> {
    ++retrieve_count_;
    return scripted_.next();
  }

  [[nodiscard]] auto retrieve_count() const noexcept -> std::size_t {
    return retrieve_count_;
  }

private:
  detail::scripted_result_queue<std::vector<std::string>> scripted_{};
  std::size_t retrieve_count_{0U};
};

} // namespace wh::testing::mock
