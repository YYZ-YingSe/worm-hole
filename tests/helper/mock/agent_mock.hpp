// Defines a deterministic mock agent used by tests to script responses
// and verify call counts/inputs.
#pragma once

#include <concepts>
#include <string>
#include <utility>

#include "helper/mock/detail/scripted_result_queue.hpp"
#include "wh/core/result.hpp"

namespace wh::testing::helper::mock {

class agent_mock {
public:
  template <typename output_t>
    requires std::constructible_from<std::string, output_t &&>
  auto enqueue_success(output_t &&output) -> void {
    scripted_.enqueue_success(std::forward<output_t>(output));
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

} // namespace wh::testing::helper::mock
