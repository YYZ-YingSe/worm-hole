#pragma once

#include <string>
#include <vector>

#include "wh/core/result.hpp"
#include "wh/testing/mock/detail/scripted_result_queue.hpp"

namespace wh::testing::mock {

class embedding_mock {
public:
  auto enqueue_success(std::vector<float> embedding) -> void {
    scripted_.enqueue_success(std::move(embedding));
  }

  auto enqueue_error(const wh::core::errc code) -> void {
    scripted_.enqueue_error(code);
  }

  [[nodiscard]] auto embed([[maybe_unused]] const std::string &text)
      -> wh::core::result<std::vector<float>> {
    ++embed_count_;
    return scripted_.next();
  }

  [[nodiscard]] auto embed_count() const noexcept -> std::size_t {
    return embed_count_;
  }

private:
  detail::scripted_result_queue<std::vector<float>> scripted_{};
  std::size_t embed_count_{0U};
};

} // namespace wh::testing::mock
