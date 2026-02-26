#pragma once

#include <string>

#include "wh/core/result.hpp"
#include "wh/testing/mock/detail/scripted_result_queue.hpp"

namespace wh::testing::mock {

class chat_model_mock {
public:
  auto enqueue_success(std::string completion) -> void {
    scripted_.enqueue_success(std::move(completion));
  }

  auto enqueue_error(const wh::core::errc code) -> void {
    scripted_.enqueue_error(code);
  }

  auto enqueue_interrupt() -> void {
    scripted_.enqueue_error(wh::core::errc::canceled);
  }

  [[nodiscard]] auto complete([[maybe_unused]] const std::string &prompt)
      -> wh::core::result<std::string> {
    ++complete_count_;
    return scripted_.next();
  }

  [[nodiscard]] auto complete_count() const noexcept -> std::size_t {
    return complete_count_;
  }

private:
  detail::scripted_result_queue<std::string> scripted_{};
  std::size_t complete_count_{0U};
};

} // namespace wh::testing::mock
