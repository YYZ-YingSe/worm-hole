// Defines the authored swarm shell that binds one explicit host agent plus a
// peer set without creating a second execution runtime.
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

/// Thin authored swarm shell that binds one entry host role and one peer set
/// onto the existing transfer topology contract.
class swarm {
public:
  /// Creates one authored swarm shell with the requested exported name.
  explicit swarm(std::string name) noexcept : name_(std::move(name)) {}

  swarm(const swarm &) = delete;
  auto operator=(const swarm &) -> swarm & = delete;
  swarm(swarm &&) noexcept = default;
  auto operator=(swarm &&) noexcept -> swarm & = default;
  ~swarm() = default;

  /// Returns the authored swarm shell name.
  [[nodiscard]] auto name() const noexcept -> std::string_view { return name_; }

  /// Returns true after the host role and peer set freeze successfully.
  [[nodiscard]] auto frozen() const noexcept -> bool { return frozen_; }

  /// Installs the explicit host role before freeze.
  auto set_host(wh::agent::role_binding value) -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    if (value.name().empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    host_.emplace(std::move(value));
    return {};
  }

  template <typename role_t>
    requires(!std::same_as<std::remove_cvref_t<role_t>, wh::agent::role_binding>)
  auto set_host(role_t &&value) -> wh::core::result<void> {
    return set_host(wh::agent::make_role_binding(std::forward<role_t>(value)));
  }

  /// Adds one peer role that will hand control back through the entry host.
  auto add_peer(wh::agent::role_binding peer) -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    if (peer.name().empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    peers_.push_back(std::move(peer));
    return {};
  }

  template <typename role_t>
    requires(!std::same_as<std::remove_cvref_t<role_t>, wh::agent::role_binding>)
  auto add_peer(role_t &&peer) -> wh::core::result<void> {
    return add_peer(wh::agent::make_role_binding(std::forward<role_t>(peer)));
  }

  /// Returns the explicit host role.
  [[nodiscard]] auto host_agent()
      -> wh::core::result<std::reference_wrapper<wh::agent::role_binding>> {
    return role_ref(host_);
  }

  /// Returns the explicit host role.
  [[nodiscard]] auto host_agent() const
      -> wh::core::result<std::reference_wrapper<const wh::agent::role_binding>> {
    return role_ref(host_);
  }

  /// Returns the current peer-name list.
  [[nodiscard]] auto peer_names() const -> std::vector<std::string> {
    std::vector<std::string> names{};
    names.reserve(peers_.size());
    for (const auto &peer : peers_) {
      names.emplace_back(peer.name());
    }
    return names;
  }

  /// Returns the peer roles used by this authored shell.
  [[nodiscard]] auto peers() noexcept -> std::vector<wh::agent::role_binding> & { return peers_; }

  /// Returns the peer roles used by this authored shell.
  [[nodiscard]] auto peers() const noexcept -> const std::vector<wh::agent::role_binding> & {
    return peers_;
  }

  /// Validates the authored shape once. Runtime topology wiring happens only
  /// when the shell is lowered into the executable agent surface.
  auto freeze() -> wh::core::result<void> {
    if (frozen_) {
      return {};
    }
    if (name_.empty() || !host_.has_value() || peers_.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    if (host_->name() != name_) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    if (!host_->executable()) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    for (const auto &peer : peers_) {
      if (!peer.executable() || peer.name().empty() || peer.name() == host_->name()) {
        return wh::core::result<void>::failure(wh::core::errc::contract_violation);
      }
    }
    for (std::size_t index = 0U; index < peers_.size(); ++index) {
      for (std::size_t other = index + 1U; other < peers_.size(); ++other) {
        if (peers_[index].name() == peers_[other].name()) {
          return wh::core::result<void>::failure(wh::core::errc::already_exists);
        }
      }
    }
    auto host_frozen = host_->freeze();
    if (host_frozen.has_error()) {
      return host_frozen;
    }
    for (auto &peer : peers_) {
      auto peer_frozen = peer.freeze();
      if (peer_frozen.has_error()) {
        return peer_frozen;
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
  std::optional<wh::agent::role_binding> host_{};
  std::vector<wh::agent::role_binding> peers_{};
  bool frozen_{false};
};

} // namespace wh::agent
