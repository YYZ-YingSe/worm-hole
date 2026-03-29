// Defines authored workflow metadata used by ADK build hooks before lowering
// to compose workflow/graph.
#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "wh/core/result.hpp"

namespace wh::adk {

/// Supported authored workflow shapes before lowering.
enum class workflow_mode {
  /// Execute steps in authored order.
  sequential = 0U,
  /// Execute sibling steps as one parallel fan-out set.
  parallel,
  /// Execute steps repeatedly until authored loop exit semantics decide to stop.
  loop,
};

/// One authored workflow step bound to one named child agent.
struct workflow_step {
  /// Stable authored step key used by future lowering and diagnostics.
  std::string key{};
  /// Stable child-agent name selected by this step.
  std::string agent_name{};
};

/// Frozen authored workflow description with compile-visible validation.
class workflow {
public:
  /// Creates one workflow authoring surface in the requested mode.
  explicit workflow(const workflow_mode mode = workflow_mode::sequential) noexcept
      : mode_(mode) {}

  workflow(const workflow &) = default;
  workflow(workflow &&) noexcept = default;
  auto operator=(const workflow &) -> workflow & = default;
  auto operator=(workflow &&) noexcept -> workflow & = default;
  ~workflow() = default;

  /// Returns the authored workflow mode.
  [[nodiscard]] auto mode() const noexcept -> workflow_mode { return mode_; }

  /// Returns true after authoring has been frozen successfully.
  [[nodiscard]] auto frozen() const noexcept -> bool { return frozen_; }

  /// Returns the optional loop iteration cap. `0` means infinite when `mode ==
  /// loop`.
  [[nodiscard]] auto max_iterations() const noexcept -> std::optional<std::size_t> {
    return max_iterations_;
  }

  /// Returns the authored step list in insertion order.
  [[nodiscard]] auto steps() const noexcept -> std::span<const workflow_step> {
    return {steps_.data(), steps_.size()};
  }

  /// Replaces the authored workflow mode before freeze.
  auto set_mode(const workflow_mode mode) -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    mode_ = mode;
    return {};
  }

  /// Sets the optional loop iteration cap before freeze.
  auto set_max_iterations(const std::optional<std::size_t> max_iterations)
      -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    max_iterations_ = max_iterations;
    return {};
  }

  /// Adds one authored workflow step before freeze.
  auto add_step(std::string key, std::string agent_name)
      -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    if (key.empty() || agent_name.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    for (const auto &step : steps_) {
      if (step.key == key) {
        return wh::core::result<void>::failure(wh::core::errc::already_exists);
      }
    }
    steps_.push_back(workflow_step{
        .key = std::move(key),
        .agent_name = std::move(agent_name),
    });
    return {};
  }

  /// Validates authored mode/step invariants and freezes the workflow.
  auto freeze() -> wh::core::result<void> {
    if (frozen_) {
      return {};
    }
    if (steps_.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    if (mode_ == workflow_mode::parallel && steps_.size() < 2U) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    if (mode_ != workflow_mode::loop && max_iterations_.has_value()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    frozen_ = true;
    return {};
  }

private:
  /// Rejects workflow mutation after freeze.
  [[nodiscard]] auto ensure_mutable() const -> wh::core::result<void> {
    if (frozen_) {
      return wh::core::result<void>::failure(
          wh::core::errc::contract_violation);
    }
    return {};
  }

  /// Authored workflow mode selected by the caller.
  workflow_mode mode_{workflow_mode::sequential};
  /// Optional loop iteration cap used only by `loop` workflows.
  std::optional<std::size_t> max_iterations_{};
  /// Ordered authored workflow steps.
  std::vector<workflow_step> steps_{};
  /// True after authoring has been frozen successfully.
  bool frozen_{false};
};

} // namespace wh::adk
