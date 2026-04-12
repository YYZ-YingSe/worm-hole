// Defines the authored reflexion shell that binds actor, critic, and optional
// memory-writer roles without creating runtime behavior.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "wh/agent/agent.hpp"
#include "wh/agent/revision.hpp"
#include "wh/core/result.hpp"

namespace wh::agent {

/// Thin authored reflexion shell that binds actor and critic roles plus one
/// optional memory-writer role.
class reflexion {
public:
  /// Creates one authored reflexion shell.
  explicit reflexion(std::string name) noexcept : name_(std::move(name)) {}

  reflexion(const reflexion &) = delete;
  auto operator=(const reflexion &) -> reflexion & = delete;
  reflexion(reflexion &&) noexcept = default;
  auto operator=(reflexion &&) noexcept -> reflexion & = default;
  ~reflexion() = default;

  /// Returns the authored shell name.
  [[nodiscard]] auto name() const noexcept -> std::string_view { return name_; }

  /// Returns true after all required roles freeze successfully.
  [[nodiscard]] auto frozen() const noexcept -> bool { return frozen_; }

  /// Installs the actor role before freeze.
  auto set_actor(agent &&actor) -> wh::core::result<void> {
    return set_role(actor_, std::move(actor));
  }

  /// Installs the critic role before freeze.
  auto set_critic(agent &&critic) -> wh::core::result<void> {
    return set_role(critic_, std::move(critic));
  }

  /// Installs the optional memory-writer role before freeze.
  auto set_memory_writer(agent &&memory_writer) -> wh::core::result<void> {
    return set_role(memory_writer_, std::move(memory_writer));
  }

  /// Replaces the maximum reflection iterations. Zero falls back to one.
  auto set_max_iterations(const std::size_t max_iterations)
      -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    max_iterations_ = max_iterations == 0U ? 1U : max_iterations;
    return {};
  }

  /// Installs the actor-request builder before freeze.
  auto set_actor_request_builder(revision_request_builder builder)
      -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    if (!builder) {
      return wh::core::result<void>::failure(
          wh::core::errc::invalid_argument);
    }
    actor_request_builder_ = std::move(builder);
    return {};
  }

  /// Installs the critic-request builder before freeze.
  auto set_critic_request_builder(revision_request_builder builder)
      -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    if (!builder) {
      return wh::core::result<void>::failure(
          wh::core::errc::invalid_argument);
    }
    critic_request_builder_ = std::move(builder);
    return {};
  }

  /// Installs the optional memory-writer request builder before freeze.
  auto set_memory_writer_request_builder(revision_request_builder builder)
      -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    if (!builder) {
      return wh::core::result<void>::failure(
          wh::core::errc::invalid_argument);
    }
    memory_writer_request_builder_ = std::move(builder);
    return {};
  }

  /// Installs the review-decision reader before freeze.
  auto set_review_decision_reader(review_decision_reader reader)
      -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    if (!reader) {
      return wh::core::result<void>::failure(
          wh::core::errc::invalid_argument);
    }
    review_decision_reader_ = std::move(reader);
    return {};
  }

  /// Returns the configured reflection budget.
  [[nodiscard]] auto max_iterations() const noexcept -> std::size_t {
    return max_iterations_;
  }

  /// Returns the actor role.
  [[nodiscard]] auto actor()
      -> wh::core::result<std::reference_wrapper<agent>> {
    return role_ref(actor_);
  }

  /// Returns the actor role.
  [[nodiscard]] auto actor() const
      -> wh::core::result<std::reference_wrapper<const agent>> {
    return role_ref(actor_);
  }

  /// Returns the critic role.
  [[nodiscard]] auto critic()
      -> wh::core::result<std::reference_wrapper<agent>> {
    return role_ref(critic_);
  }

  /// Returns the critic role.
  [[nodiscard]] auto critic() const
      -> wh::core::result<std::reference_wrapper<const agent>> {
    return role_ref(critic_);
  }

  /// Returns the optional memory-writer role.
  [[nodiscard]] auto memory_writer()
      -> wh::core::result<std::reference_wrapper<agent>> {
    return role_ref(memory_writer_);
  }

  /// Returns the optional memory-writer role.
  [[nodiscard]] auto memory_writer() const
      -> wh::core::result<std::reference_wrapper<const agent>> {
    return role_ref(memory_writer_);
  }

  /// Returns the actor-request builder.
  [[nodiscard]] auto actor_request_builder() const noexcept
      -> const revision_request_builder & {
    return actor_request_builder_;
  }

  /// Returns the critic-request builder.
  [[nodiscard]] auto critic_request_builder() const noexcept
      -> const revision_request_builder & {
    return critic_request_builder_;
  }

  /// Returns the optional memory-writer request builder.
  [[nodiscard]] auto memory_writer_request_builder() const noexcept
      -> const revision_request_builder & {
    return memory_writer_request_builder_;
  }

  /// Returns the review-decision reader.
  [[nodiscard]] auto review_decision_reader() const noexcept
      -> const wh::agent::review_decision_reader & {
    return review_decision_reader_;
  }

  /// Freezes all installed roles.
  auto freeze() -> wh::core::result<void> {
    if (frozen_) {
      return {};
    }
    if (name_.empty() || !actor_.has_value() || !critic_.has_value()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    if (!actor_request_builder_ || !critic_request_builder_ ||
        !review_decision_reader_) {
      return wh::core::result<void>::failure(
          wh::core::errc::contract_violation);
    }
    if (memory_writer_.has_value() !=
        static_cast<bool>(memory_writer_request_builder_)) {
      return wh::core::result<void>::failure(
          wh::core::errc::contract_violation);
    }
    if (!actor_->executable() || !critic_->executable()) {
      return wh::core::result<void>::failure(
          wh::core::errc::contract_violation);
    }
    if (memory_writer_.has_value() && !memory_writer_->executable()) {
      return wh::core::result<void>::failure(
          wh::core::errc::contract_violation);
    }
    auto actor_frozen = actor_->freeze();
    if (actor_frozen.has_error()) {
      return actor_frozen;
    }
    auto critic_frozen = critic_->freeze();
    if (critic_frozen.has_error()) {
      return critic_frozen;
    }
    if (memory_writer_.has_value()) {
      auto memory_writer_frozen = memory_writer_->freeze();
      if (memory_writer_frozen.has_error()) {
        return memory_writer_frozen;
      }
    }
    frozen_ = true;
    return {};
  }

  /// Converts this authored shell into the common executable agent surface.
  [[nodiscard]] auto into_agent() && -> wh::core::result<wh::agent::agent>;

private:
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
      return wh::core::result<std::reference_wrapper<agent>>::failure(
          wh::core::errc::not_found);
    }
    return std::ref(*slot);
  }

  [[nodiscard]] auto ensure_mutable() const -> wh::core::result<void> {
    if (frozen_) {
      return wh::core::result<void>::failure(
          wh::core::errc::contract_violation);
    }
    return {};
  }

  std::string name_{};
  std::optional<agent> actor_{};
  std::optional<agent> critic_{};
  std::optional<agent> memory_writer_{};
  std::size_t max_iterations_{3U};
  revision_request_builder actor_request_builder_{nullptr};
  revision_request_builder critic_request_builder_{nullptr};
  revision_request_builder memory_writer_request_builder_{nullptr};
  wh::agent::review_decision_reader review_decision_reader_{nullptr};
  bool frozen_{false};
};

} // namespace wh::agent
