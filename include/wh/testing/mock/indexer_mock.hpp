#pragma once

#include <string>
#include <vector>

#include "wh/core/result.hpp"
#include "wh/testing/mock/detail/scripted_result_queue.hpp"

namespace wh::testing::mock {

class indexer_mock {
public:
  auto enqueue_success() -> void { scripted_.enqueue_success(); }

  auto enqueue_error(const wh::core::errc code) -> void {
    scripted_.enqueue_error(code);
  }

  [[nodiscard]] auto
  upsert([[maybe_unused]] const std::string &document_id,
         [[maybe_unused]] const std::vector<float> &embedding)
      -> wh::core::result<void> {
    ++upsert_count_;
    return scripted_.next();
  }

  [[nodiscard]] auto upsert_count() const noexcept -> std::size_t {
    return upsert_count_;
  }

private:
  detail::scripted_result_queue<void> scripted_{};
  std::size_t upsert_count_{0U};
};

} // namespace wh::testing::mock
