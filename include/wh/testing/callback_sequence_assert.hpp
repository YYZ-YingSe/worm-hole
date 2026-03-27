// Defines testing helpers that assert callback invocation order and payload
// sequences in deterministic unit tests.
#pragma once

#include <initializer_list>
#include <ranges>
#include <span>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/small_vector.hpp"
#include "wh/core/callback.hpp"

namespace wh::testing {

/// Public interface for `callback_sequence_assert`.
class callback_sequence_assert {
public:
  /// Appends one callback stage event to the captured sequence.
  auto record(const wh::core::callback_stage stage) -> void {
    stages_.push_back(stage);
  }

  /// Returns immutable view of recorded callback stages.
  [[nodiscard]] auto stages() const noexcept
      -> std::span<const wh::core::callback_stage> {
    return {stages_.data(), stages_.size()};
  }

  /// Validates recorded callback stages against an expected ordered sequence.
  [[nodiscard]] auto
  expect(const std::initializer_list<wh::core::callback_stage> expected) const
      -> wh::core::result<void> {
    if (expected.size() != stages_.size()) {
      return wh::core::result<void>::failure(
          wh::core::errc::contract_violation);
    }
    const auto matches = std::ranges::equal(expected, stages_);
    if (!matches) {
      return wh::core::result<void>::failure(
          wh::core::errc::contract_violation);
    }
    return {};
  }

private:
  /// Recorded callback stage sequence in observed order.
  wh::core::small_vector<wh::core::callback_stage, 16U> stages_{};
};

} // namespace wh::testing
