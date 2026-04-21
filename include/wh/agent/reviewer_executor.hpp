// Defines the authored reviewer-executor shell that binds one reviewer agent
// and one executor agent without introducing runtime behavior.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "wh/agent/agent.hpp"
#include "wh/agent/revision.hpp"
#include "wh/core/result.hpp"

namespace wh::agent {

/// Thin authored reviewer-executor shell that binds one reviewer role and one
/// executor role.
class reviewer_executor {
public:
  /// Creates one authored reviewer-executor shell.
  explicit reviewer_executor(std::string name) noexcept : name_(std::move(name)) {}

  reviewer_executor(const reviewer_executor &) = delete;
  auto operator=(const reviewer_executor &) -> reviewer_executor & = delete;
  reviewer_executor(reviewer_executor &&) noexcept = default;
  auto operator=(reviewer_executor &&) noexcept -> reviewer_executor & = default;
  ~reviewer_executor() = default;

  /// Returns the authored shell name.
  [[nodiscard]] auto name() const noexcept -> std::string_view { return name_; }

  /// Returns true after all required roles freeze successfully.
  [[nodiscard]] auto frozen() const noexcept -> bool { return frozen_; }

  /// Installs the reviewer role before freeze.
  auto set_reviewer(agent &&reviewer) -> wh::core::result<void> {
    return set_role(reviewer_, std::move(reviewer));
  }

  /// Installs the executor role before freeze.
  auto set_executor(agent &&executor) -> wh::core::result<void> {
    return set_role(executor_, std::move(executor));
  }

  /// Replaces the maximum review-revise iterations. Zero falls back to one.
  auto set_max_iterations(const std::size_t max_iterations) -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    max_iterations_ = max_iterations == 0U ? 1U : max_iterations;
    return {};
  }

  /// Installs the executor-request builder before freeze.
  auto set_executor_request_builder(revision_request_builder builder) -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    if (!builder) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    executor_request_builder_ = std::move(builder);
    return {};
  }

  /// Installs the reviewer-request builder before freeze.
  auto set_reviewer_request_builder(revision_request_builder builder) -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    if (!builder) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    reviewer_request_builder_ = std::move(builder);
    return {};
  }

  /// Installs the review-decision reader before freeze.
  auto set_review_decision_reader(review_decision_reader reader) -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    if (!reader) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    review_decision_reader_ = std::move(reader);
    return {};
  }

  /// Returns the frozen reviewer role.
  [[nodiscard]] auto reviewer() -> wh::core::result<std::reference_wrapper<agent>> {
    return role_ref(reviewer_);
  }

  /// Returns the frozen reviewer role.
  [[nodiscard]] auto reviewer() const -> wh::core::result<std::reference_wrapper<const agent>> {
    return role_ref(reviewer_);
  }

  /// Returns the frozen executor role.
  [[nodiscard]] auto executor() -> wh::core::result<std::reference_wrapper<agent>> {
    return role_ref(executor_);
  }

  /// Returns the frozen executor role.
  [[nodiscard]] auto executor() const -> wh::core::result<std::reference_wrapper<const agent>> {
    return role_ref(executor_);
  }

  /// Returns the configured review-revise budget.
  [[nodiscard]] auto max_iterations() const noexcept -> std::size_t { return max_iterations_; }

  /// Returns the executor-request builder.
  [[nodiscard]] auto executor_request_builder() const noexcept -> const revision_request_builder & {
    return executor_request_builder_;
  }

  /// Returns the reviewer-request builder.
  [[nodiscard]] auto reviewer_request_builder() const noexcept -> const revision_request_builder & {
    return reviewer_request_builder_;
  }

  /// Returns the review-decision reader.
  [[nodiscard]] auto review_decision_reader() const noexcept
      -> const wh::agent::review_decision_reader & {
    return review_decision_reader_;
  }

  /// Validates role completeness and freezes all installed roles.
  auto freeze() -> wh::core::result<void> {
    if (frozen_) {
      return {};
    }
    if (name_.empty() || !reviewer_.has_value() || !executor_.has_value()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    if (!executor_request_builder_ || !reviewer_request_builder_ || !review_decision_reader_) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    if (reviewer_->name() == executor_->name()) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    if (!reviewer_->executable() || !executor_->executable()) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    auto reviewer_frozen = reviewer_->freeze();
    if (reviewer_frozen.has_error()) {
      return reviewer_frozen;
    }
    auto executor_frozen = executor_->freeze();
    if (executor_frozen.has_error()) {
      return executor_frozen;
    }
    frozen_ = true;
    return {};
  }

  /// Converts this frozen authored shell into the common executable agent surface.
  [[nodiscard]] auto into_agent() && -> wh::core::result<wh::agent::agent>;

private:
  auto set_role(std::optional<agent> &slot, agent &&value) -> wh::core::result<void> {
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
  std::optional<agent> reviewer_{};
  std::optional<agent> executor_{};
  std::size_t max_iterations_{3U};
  revision_request_builder executor_request_builder_{nullptr};
  revision_request_builder reviewer_request_builder_{nullptr};
  wh::agent::review_decision_reader review_decision_reader_{nullptr};
  bool frozen_{false};
};

} // namespace wh::agent
