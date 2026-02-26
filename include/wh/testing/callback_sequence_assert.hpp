#pragma once

#include <initializer_list>
#include <ranges>
#include <span>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/small_vector.hpp"
#include "wh/core/types/callback_types.hpp"

namespace wh::testing {

class callback_sequence_assert {
public:
  auto record(const wh::core::callback_stage stage) -> void {
    const auto pushed = stages_.push_back(stage);
    static_cast<void>(pushed);
  }

  [[nodiscard]] auto stages() const noexcept
      -> std::span<const wh::core::callback_stage> {
    return {stages_.data(), stages_.size()};
  }

  [[nodiscard]] auto expect(
      const std::initializer_list<wh::core::callback_stage> expected) const
      -> wh::core::result<void> {
    if (expected.size() != stages_.size()) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    const auto matches = std::ranges::equal(expected, stages_);
    if (!matches) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    return {};
  }

private:
  wh::core::small_vector<wh::core::callback_stage, 16U> stages_{};
};

} // namespace wh::testing
