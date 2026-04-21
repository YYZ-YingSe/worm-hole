// Defines a deterministic retriever mock for scripted ranked result sequences.
#pragma once

#include <concepts>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "helper/mock/detail/scripted_result_queue.hpp"
#include "wh/core/result.hpp"

namespace wh::testing::helper::mock {

class retriever_mock {
public:
  template <typename documents_t>
    requires std::constructible_from<std::vector<std::string>, documents_t &&>
  auto enqueue_success(documents_t &&documents) -> void {
    scripted_.enqueue_success(std::forward<documents_t>(documents));
  }

  auto enqueue_success(std::initializer_list<std::string> documents) -> void {
    scripted_.enqueue_success(std::vector<std::string>{documents});
  }

  auto enqueue_error(const wh::core::errc code) -> void { scripted_.enqueue_error(code); }

  [[nodiscard]] auto retrieve([[maybe_unused]] const std::string &query)
      -> wh::core::result<std::vector<std::string>> {
    ++retrieve_count_;
    return scripted_.next();
  }

  [[nodiscard]] auto retrieve_count() const noexcept -> std::size_t { return retrieve_count_; }

private:
  detail::scripted_result_queue<std::vector<std::string>> scripted_{};
  std::size_t retrieve_count_{0U};
};

} // namespace wh::testing::helper::mock
