// Defines the authored research shell as one lead-and-specialist template
// built on top of the common transfer topology contract.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "wh/agent/agent.hpp"
#include "wh/core/result.hpp"

namespace wh::agent {

/// Thin authored research shell that binds one lead agent plus optional
/// specialist agents and later lowers onto the common executable agent
/// surface.
class research {
public:
  /// Creates one authored research shell.
  explicit research(std::string name) noexcept : name_(std::move(name)) {}

  research(const research &) = delete;
  auto operator=(const research &) -> research & = delete;
  research(research &&) noexcept = default;
  auto operator=(research &&) noexcept -> research & = default;
  ~research() = default;

  /// Returns the authored shell name.
  [[nodiscard]] auto name() const noexcept -> std::string_view { return name_; }

  /// Returns true after the lead and specialist set freeze successfully.
  [[nodiscard]] auto frozen() const noexcept -> bool { return frozen_; }

  /// Installs the lead researcher before freeze.
  auto set_lead(agent &&lead) -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    if (lead.name().empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    lead_.emplace(std::move(lead));
    return {};
  }

  /// Adds one specialist peer before freeze.
  auto add_specialist(agent &&specialist) -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    if (specialist.name().empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    specialists_.push_back(std::move(specialist));
    return {};
  }

  /// Returns the lead researcher role.
  [[nodiscard]] auto lead() -> wh::core::result<std::reference_wrapper<agent>> {
    return role_ref(lead_);
  }

  /// Returns the lead researcher role.
  [[nodiscard]] auto lead() const -> wh::core::result<std::reference_wrapper<const agent>> {
    return role_ref(lead_);
  }

  /// Returns the current specialist-name list.
  [[nodiscard]] auto specialist_names() const -> std::vector<std::string> {
    std::vector<std::string> names{};
    names.reserve(specialists_.size());
    for (const auto &specialist : specialists_) {
      names.emplace_back(specialist.name());
    }
    return names;
  }

  /// Returns the specialist roles used by this authored shell.
  [[nodiscard]] auto specialists() noexcept -> std::vector<agent> & { return specialists_; }

  /// Returns the specialist roles used by this authored shell.
  [[nodiscard]] auto specialists() const noexcept -> const std::vector<agent> & {
    return specialists_;
  }

  /// Validates the authored template shape and freezes all bound roles.
  auto freeze() -> wh::core::result<void> {
    if (frozen_) {
      return {};
    }
    if (name_.empty() || !lead_.has_value()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    if (lead_->name() != name_) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    if (!lead_->executable()) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    for (const auto &specialist : specialists_) {
      if (!specialist.executable() || specialist.name().empty() ||
          specialist.name() == lead_->name()) {
        return wh::core::result<void>::failure(wh::core::errc::contract_violation);
      }
    }
    for (std::size_t index = 0U; index < specialists_.size(); ++index) {
      for (std::size_t other = index + 1U; other < specialists_.size(); ++other) {
        if (specialists_[index].name() == specialists_[other].name()) {
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
  std::optional<agent> lead_{};
  std::vector<agent> specialists_{};
  bool frozen_{false};
};

} // namespace wh::agent
