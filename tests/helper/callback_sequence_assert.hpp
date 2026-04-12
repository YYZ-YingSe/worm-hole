// Defines testing helpers that assert callback invocation order and payload
// sequences in deterministic unit tests.
#pragma once

#include <initializer_list>
#include <ranges>
#include <span>

#include "wh/core/callback.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/small_vector.hpp"

namespace wh::testing::helper {

class callback_sequence_assert {
public:
  auto record(const wh::core::callback_stage stage) -> void {
    stages_.push_back(stage);
  }

  [[nodiscard]] auto stages() const noexcept
      -> std::span<const wh::core::callback_stage> {
    return {stages_.data(), stages_.size()};
  }

  [[nodiscard]] auto
  expect(const std::initializer_list<wh::core::callback_stage> expected) const
      -> wh::core::result<void> {
    if (expected.size() != stages_.size()) {
      return wh::core::result<void>::failure(
          wh::core::errc::contract_violation);
    }
    if (!std::ranges::equal(expected, stages_)) {
      return wh::core::result<void>::failure(
          wh::core::errc::contract_violation);
    }
    return {};
  }

private:
  wh::core::small_vector<wh::core::callback_stage, 16U> stages_{};
};

} // namespace wh::testing::helper
