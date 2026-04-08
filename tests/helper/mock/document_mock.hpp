// Defines a deterministic document mock used to script document operation
// outputs in tests.
#pragma once

#include <concepts>
#include <string>
#include <utility>

#include "helper/mock/detail/scripted_result_queue.hpp"
#include "wh/core/result.hpp"

namespace wh::testing::helper::mock {

class document_mock {
public:
  template <typename content_t>
    requires std::constructible_from<std::string, content_t &&>
  auto enqueue_success(content_t &&content) -> void {
    scripted_.enqueue_success(std::forward<content_t>(content));
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

} // namespace wh::testing::helper::mock
