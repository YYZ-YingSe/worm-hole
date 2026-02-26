#pragma once

#include <string>

#include "wh/core/result.hpp"
#include "wh/testing/mock/detail/scripted_result_queue.hpp"

namespace wh::testing::mock {

class document_mock {
public:
  auto enqueue_success(std::string content) -> void {
    scripted_.enqueue_success(std::move(content));
  }

  auto enqueue_error(const wh::core::errc code) -> void {
    scripted_.enqueue_error(code);
  }

  [[nodiscard]] auto load([[maybe_unused]] const std::string &document_id)
      -> wh::core::result<std::string> {
    ++load_count_;
    return scripted_.next();
  }

  [[nodiscard]] auto load_count() const noexcept -> std::size_t {
    return load_count_;
  }

private:
  detail::scripted_result_queue<std::string> scripted_{};
  std::size_t load_count_{0U};
};

} // namespace wh::testing::mock
