// Defines the authored research shell as one lead-and-specialist template
// built on top of the common transfer topology contract.
#pragma once

#include <concepts>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "wh/agent/role_binding.hpp"
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
  auto set_lead(wh::agent::role_binding lead) -> wh::core::result<void> {
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

  template <typename role_t>
    requires(!std::same_as<std::remove_cvref_t<role_t>, wh::agent::role_binding>)
  auto set_lead(role_t &&lead) -> wh::core::result<void> {
    return set_lead(wh::agent::make_role_binding(std::forward<role_t>(lead)));
  }

  /// Adds one specialist peer before freeze.
  auto add_specialist(wh::agent::role_binding specialist) -> wh::core::result<void> {
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

  template <typename role_t>
    requires(!std::same_as<std::remove_cvref_t<role_t>, wh::agent::role_binding>)
  auto add_specialist(role_t &&specialist) -> wh::core::result<void> {
    return add_specialist(wh::agent::make_role_binding(std::forward<role_t>(specialist)));
  }

  /// Returns the lead researcher role.
  [[nodiscard]] auto lead() -> wh::core::result<std::reference_wrapper<wh::agent::role_binding>> {
    return role_ref(lead_);
  }

  /// Returns the lead researcher role.
  [[nodiscard]] auto lead() const
      -> wh::core::result<std::reference_wrapper<const wh::agent::role_binding>> {
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
  [[nodiscard]] auto specialists() noexcept -> std::vector<wh::agent::role_binding> & {
    return specialists_;
  }

  /// Returns the specialist roles used by this authored shell.
  [[nodiscard]] auto specialists() const noexcept -> const std::vector<wh::agent::role_binding> & {
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
    auto lead_frozen = lead_->freeze();
    if (lead_frozen.has_error()) {
      return lead_frozen;
    }
    for (auto &specialist : specialists_) {
      auto specialist_frozen = specialist.freeze();
      if (specialist_frozen.has_error()) {
        return specialist_frozen;
      }
    }
    frozen_ = true;
    return {};
  }

  /// Converts this frozen authored shell into the common executable agent surface.
  [[nodiscard]] auto into_agent() && -> wh::core::result<wh::agent::agent>;

private:
  [[nodiscard]] static auto role_ref(const std::optional<wh::agent::role_binding> &slot)
      -> wh::core::result<std::reference_wrapper<const wh::agent::role_binding>> {
    if (!slot.has_value()) {
      return wh::core::result<std::reference_wrapper<const wh::agent::role_binding>>::failure(
          wh::core::errc::not_found);
    }
    return std::cref(*slot);
  }

  [[nodiscard]] static auto role_ref(std::optional<wh::agent::role_binding> &slot)
      -> wh::core::result<std::reference_wrapper<wh::agent::role_binding>> {
    if (!slot.has_value()) {
      return wh::core::result<std::reference_wrapper<wh::agent::role_binding>>::failure(
          wh::core::errc::not_found);
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
  std::optional<wh::agent::role_binding> lead_{};
  std::vector<wh::agent::role_binding> specialists_{};
  bool frozen_{false};
};

} // namespace wh::agent
