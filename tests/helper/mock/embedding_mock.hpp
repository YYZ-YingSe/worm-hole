// Defines a scripted embedding mock for deterministic vector generation
// behavior in tests.
#pragma once

#include <concepts>
#include <string>
#include <utility>
#include <vector>

#include "helper/mock/detail/scripted_result_queue.hpp"
#include "wh/core/result.hpp"

namespace wh::testing::helper::mock {

class embedding_mock {
public:
  template <typename embedding_t>
    requires std::constructible_from<std::vector<float>, embedding_t &&>
  auto enqueue_success(embedding_t &&embedding) -> void {
    scripted_.enqueue_success(std::forward<embedding_t>(embedding));
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

} // namespace wh::testing::helper::mock
