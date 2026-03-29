// Defines the authored plan-execute template hook used by Task 14 to freeze
// planner/executor/replanner metadata before Task 15 lowering.
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "wh/agent/agent.hpp"
#include "wh/core/result.hpp"

namespace wh::agent {

/// Thin authored plan-execute template that binds planner/executor/replanner
/// roles without introducing runtime behavior.
class plan_execute {
public:
  /// Creates one authored plan-execute template shell.
  explicit plan_execute(std::string name) noexcept : name_(std::move(name)) {}

  plan_execute(const plan_execute &) = delete;
  auto operator=(const plan_execute &) -> plan_execute & = delete;
  plan_execute(plan_execute &&) noexcept = default;
  auto operator=(plan_execute &&) noexcept -> plan_execute & = default;
  ~plan_execute() = default;

  /// Returns the authored template name.
  [[nodiscard]] auto name() const noexcept -> std::string_view { return name_; }

  /// Returns true after all required authored roles freeze successfully.
  [[nodiscard]] auto frozen() const noexcept -> bool { return frozen_; }

  /// Installs the planner role before freeze.
  auto set_planner(agent &&planner) -> wh::core::result<void> {
    return set_role(planner_, std::move(planner));
  }

  /// Installs the executor role before freeze.
  auto set_executor(agent &&executor) -> wh::core::result<void> {
    return set_role(executor_, std::move(executor));
  }

  /// Installs the optional replanner role before freeze.
  auto set_replanner(agent &&replanner) -> wh::core::result<void> {
    return set_role(replanner_, std::move(replanner));
  }

  /// Returns the frozen planner role.
  [[nodiscard]] auto planner() const
      -> wh::core::result<std::reference_wrapper<const agent>> {
    return role_ref(planner_);
  }

  /// Returns the frozen executor role.
  [[nodiscard]] auto executor() const
      -> wh::core::result<std::reference_wrapper<const agent>> {
    return role_ref(executor_);
  }

  /// Returns the optional replanner role when present.
  [[nodiscard]] auto replanner() const
      -> wh::core::result<std::reference_wrapper<const agent>> {
    return role_ref(replanner_);
  }

  /// Returns the effective replanner name. When no explicit replanner is
  /// installed, the planner role is reused.
  [[nodiscard]] auto effective_replanner_name() const
      -> wh::core::result<std::string_view> {
    if (replanner_.has_value()) {
      return std::string_view{replanner_->name()};
    }
    auto planner_role = planner();
    if (planner_role.has_error()) {
      return wh::core::result<std::string_view>::failure(planner_role.error());
    }
    return std::string_view{planner_role.value().get().name()};
  }

  /// Validates authored role completeness and freezes all installed roles.
  auto freeze() -> wh::core::result<void> {
    if (frozen_) {
      return {};
    }
    if (name_.empty() || !planner_.has_value() || !executor_.has_value()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    if (planner_->name() == executor_->name()) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    auto planner_frozen = planner_->freeze();
    if (planner_frozen.has_error()) {
      return planner_frozen;
    }
    auto executor_frozen = executor_->freeze();
    if (executor_frozen.has_error()) {
      return executor_frozen;
    }
    if (replanner_.has_value()) {
      auto replanner_frozen = replanner_->freeze();
      if (replanner_frozen.has_error()) {
        return replanner_frozen;
      }
    }
    frozen_ = true;
    return {};
  }

private:
  /// Installs one role before freeze.
  auto set_role(std::optional<agent> &slot, agent &&value)
      -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    if (value.name().empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    slot.emplace(std::move(value));
    return {};
  }

  /// Returns one installed role by reference.
  [[nodiscard]] static auto role_ref(const std::optional<agent> &slot)
      -> wh::core::result<std::reference_wrapper<const agent>> {
    if (!slot.has_value()) {
      return wh::core::result<std::reference_wrapper<const agent>>::failure(
          wh::core::errc::not_found);
    }
    return std::cref(*slot);
  }

  /// Rejects role mutation after freeze.
  [[nodiscard]] auto ensure_mutable() const -> wh::core::result<void> {
    if (frozen_) {
      return wh::core::result<void>::failure(
          wh::core::errc::contract_violation);
    }
    return {};
  }

  /// Stable authored template name.
  std::string name_{};
  /// Planner role used to author planning turns.
  std::optional<agent> planner_{};
  /// Executor role used to author execution turns.
  std::optional<agent> executor_{};
  /// Optional replanner role used after execution feedback.
  std::optional<agent> replanner_{};
  /// True after all authored roles have been frozen successfully.
  bool frozen_{false};
};

} // namespace wh::agent
