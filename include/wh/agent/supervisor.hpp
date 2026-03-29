// Defines the authored supervisor template hook used by Task 14 to freeze a
// supervisor root and its worker-agent set without executing them.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "wh/agent/agent.hpp"
#include "wh/core/result.hpp"

namespace wh::agent {

/// Thin authored supervisor template that owns one supervisor root and a set
/// of worker child agents constrained to return upward.
class supervisor {
public:
  /// Creates one authored supervisor shell with the requested root name.
  explicit supervisor(std::string name) noexcept
      : name_(std::move(name)), root_(name_) {}

  supervisor(const supervisor &) = delete;
  auto operator=(const supervisor &) -> supervisor & = delete;
  supervisor(supervisor &&) noexcept = default;
  auto operator=(supervisor &&) noexcept -> supervisor & = default;
  ~supervisor() = default;

  /// Returns the authored supervisor name.
  [[nodiscard]] auto name() const noexcept -> std::string_view { return name_; }

  /// Returns true after worker bindings and root topology freeze successfully.
  [[nodiscard]] auto frozen() const noexcept -> bool { return frozen_; }

  /// Adds one worker and automatically enables upward transfer back to the
  /// supervisor root.
  auto add_worker(agent &&worker) -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    const auto worker_name = std::string{worker.name()};
    auto upward = worker.allow_transfer_to_parent();
    if (upward.has_error()) {
      return upward;
    }
    auto added = root_.add_child(std::move(worker));
    if (added.has_error()) {
      return added;
    }
    return root_.allow_transfer_to_child(worker_name);
  }

  /// Returns the authored supervisor root agent.
  [[nodiscard]] auto root_agent() const noexcept -> const agent & { return root_; }

  /// Returns the current worker-name list.
  [[nodiscard]] auto worker_names() const -> std::vector<std::string> {
    return root_.child_names();
  }

  /// Validates worker presence and freezes the supervisor root.
  auto freeze() -> wh::core::result<void> {
    if (frozen_) {
      return {};
    }
    if (name_.empty() || root_.child_count() == 0U) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    auto root_frozen = root_.freeze();
    if (root_frozen.has_error()) {
      return root_frozen;
    }
    frozen_ = true;
    return {};
  }

private:
  /// Rejects worker mutation after freeze.
  [[nodiscard]] auto ensure_mutable() const -> wh::core::result<void> {
    if (frozen_) {
      return wh::core::result<void>::failure(
          wh::core::errc::contract_violation);
    }
    return {};
  }

  /// Stable supervisor root name.
  std::string name_{};
  /// Root authored agent that owns the worker-agent set.
  agent root_{""};
  /// True after the supervisor root has been frozen successfully.
  bool frozen_{false};
};

} // namespace wh::agent
