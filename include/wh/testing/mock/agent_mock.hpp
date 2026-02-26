#pragma once

#include <string>

#include "wh/core/result.hpp"
#include "wh/testing/mock/detail/scripted_result_queue.hpp"

namespace wh::testing::mock {

class agent_mock {
public:
  auto enqueue_success(std::string output) -> void {
    scripted_.enqueue_success(std::move(output));
  }

  auto enqueue_error(const wh::core::errc code) -> void {
    scripted_.enqueue_error(code);
  }

  auto enqueue_interrupt() -> void {
    scripted_.enqueue_error(wh::core::errc::canceled);
  }

  [[nodiscard]] auto run([[maybe_unused]] const std::string &input)
      -> wh::core::result<std::string> {
    ++run_count_;
    return scripted_.next();
  }

  [[nodiscard]] auto run_count() const noexcept -> std::size_t {
    return run_count_;
  }

private:
  detail::scripted_result_queue<std::string> scripted_{};
  std::size_t run_count_{0U};
};

} // namespace wh::testing::mock
