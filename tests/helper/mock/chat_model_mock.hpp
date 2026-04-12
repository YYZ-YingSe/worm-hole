// Defines a scripted chat model mock that queues success/error outcomes
// for deterministic chat model testing.
#pragma once

#include <concepts>
#include <string>
#include <utility>

#include "helper/mock/detail/scripted_result_queue.hpp"
#include "wh/core/result.hpp"

namespace wh::testing::helper::mock {

class chat_model_mock {
public:
  template <typename completion_t>
    requires std::constructible_from<std::string, completion_t &&>
  auto enqueue_success(completion_t &&completion) -> void {
    scripted_.enqueue_success(std::forward<completion_t>(completion));
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

} // namespace wh::testing::helper::mock
