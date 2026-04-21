// Defines the authored supervisor shell that binds one explicit supervisor
// agent plus a worker set without creating a second execution runtime.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "wh/agent/agent.hpp"
#include "wh/core/result.hpp"

namespace wh::agent {

/// Thin authored supervisor shell that binds one explicit coordinator role and
/// one worker set onto the existing transfer topology contract.
class supervisor {
public:
  /// Creates one authored supervisor shell with the requested exported name.
  explicit supervisor(std::string name) noexcept : name_(std::move(name)) {}

  supervisor(const supervisor &) = delete;
  auto operator=(const supervisor &) -> supervisor & = delete;
  supervisor(supervisor &&) noexcept = default;
  auto operator=(supervisor &&) noexcept -> supervisor & = default;
  ~supervisor() = default;

  /// Returns the authored supervisor shell name.
  [[nodiscard]] auto name() const noexcept -> std::string_view { return name_; }

  /// Returns true after the supervisor role and worker set freeze successfully.
  [[nodiscard]] auto frozen() const noexcept -> bool { return frozen_; }

  /// Installs the explicit supervisor role before freeze.
  auto set_supervisor(agent &&value) -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    if (value.name().empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    supervisor_.emplace(std::move(value));
    return {};
  }

  /// Adds one worker role that will be constrained to transfer only back to
  /// the supervisor.
  auto add_worker(agent &&worker) -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    if (worker.name().empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    workers_.push_back(std::move(worker));
    return {};
  }

  /// Returns the explicit supervisor role.
  [[nodiscard]] auto supervisor_agent() -> wh::core::result<std::reference_wrapper<agent>> {
    return role_ref(supervisor_);
  }

  /// Returns the explicit supervisor role.
  [[nodiscard]] auto supervisor_agent() const
      -> wh::core::result<std::reference_wrapper<const agent>> {
    return role_ref(supervisor_);
  }

  /// Returns the current worker-name list.
  [[nodiscard]] auto worker_names() const -> std::vector<std::string> {
    std::vector<std::string> names{};
    names.reserve(workers_.size());
    for (const auto &worker : workers_) {
      names.emplace_back(worker.name());
    }
    return names;
  }

  /// Returns the worker roles used by this authored shell.
  [[nodiscard]] auto workers() noexcept -> std::vector<agent> & { return workers_; }

  /// Returns the worker roles used by this authored shell.
  [[nodiscard]] auto workers() const noexcept -> const std::vector<agent> & { return workers_; }

  /// Validates the authored shape once. Runtime topology wiring happens only
  /// when the shell is lowered into the executable agent surface.
  auto freeze() -> wh::core::result<void> {
    if (frozen_) {
      return {};
    }
    if (name_.empty() || !supervisor_.has_value() || workers_.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    if (supervisor_->name() != name_) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    if (!supervisor_->executable()) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    for (const auto &worker : workers_) {
      if (!worker.executable() || worker.name().empty() || worker.name() == supervisor_->name()) {
        return wh::core::result<void>::failure(wh::core::errc::contract_violation);
      }
    }
    for (std::size_t index = 0U; index < workers_.size(); ++index) {
      for (std::size_t other = index + 1U; other < workers_.size(); ++other) {
        if (workers_[index].name() == workers_[other].name()) {
          return wh::core::result<void>::failure(wh::core::errc::already_exists);
        }
      }
    }
    frozen_ = true;
    return {};
  }

  /// Converts this frozen authored shell into the common executable agent surface.
  [[nodiscard]] auto into_agent() && -> wh::core::result<wh::agent::agent>;

private:
  [[nodiscard]] static auto role_ref(const std::optional<agent> &slot)
      -> wh::core::result<std::reference_wrapper<const agent>> {
    if (!slot.has_value()) {
      return wh::core::result<std::reference_wrapper<const agent>>::failure(
          wh::core::errc::not_found);
    }
    return std::cref(*slot);
  }

  [[nodiscard]] static auto role_ref(std::optional<agent> &slot)
      -> wh::core::result<std::reference_wrapper<agent>> {
    if (!slot.has_value()) {
      return wh::core::result<std::reference_wrapper<agent>>::failure(wh::core::errc::not_found);
    }
    return std::ref(*slot);
  }

  [[nodiscard]] auto ensure_mutable() const -> wh::core::result<void> {
    if (frozen_) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    return {};
  }

  std::string name_{};
  std::optional<agent> supervisor_{};
  std::vector<agent> workers_{};
  bool frozen_{false};
};

} // namespace wh::agent
