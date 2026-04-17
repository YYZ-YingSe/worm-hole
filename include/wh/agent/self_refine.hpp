// Defines the authored self-refine shell for one agent that iterates against
// its own feedback without creating runtime behavior.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "wh/agent/agent.hpp"
#include "wh/agent/revision.hpp"
#include "wh/core/result.hpp"

namespace wh::agent {

/// Thin authored self-refine shell that binds one worker role and one optional
/// reviewer role reused across refinement iterations.
class self_refine {
public:
  /// Creates one authored self-refine shell.
  explicit self_refine(std::string name) noexcept : name_(std::move(name)) {}

  self_refine(const self_refine &) = delete;
  auto operator=(const self_refine &) -> self_refine & = delete;
  self_refine(self_refine &&) noexcept = default;
  auto operator=(self_refine &&) noexcept -> self_refine & = default;
  ~self_refine() = default;

  /// Returns the authored shell name.
  [[nodiscard]] auto name() const noexcept -> std::string_view { return name_; }

  /// Returns true after all required roles freeze successfully.
  [[nodiscard]] auto frozen() const noexcept -> bool { return frozen_; }

  /// Installs the worker role before freeze.
  auto set_worker(agent &&worker) -> wh::core::result<void> {
    return set_role(worker_, std::move(worker));
  }

  /// Installs the optional reviewer role before freeze.
  auto set_reviewer(agent &&reviewer) -> wh::core::result<void> {
    return set_role(reviewer_, std::move(reviewer));
  }

  /// Replaces the maximum refinement iterations. Zero falls back to one.
  auto set_max_iterations(const std::size_t max_iterations)
      -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    max_iterations_ = max_iterations == 0U ? 1U : max_iterations;
    return {};
  }

  /// Installs the worker-request builder before freeze.
  auto set_worker_request_builder(revision_request_builder builder)
      -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    if (!builder) {
      return wh::core::result<void>::failure(
          wh::core::errc::invalid_argument);
    }
    worker_request_builder_ = std::move(builder);
    return {};
  }

  /// Installs the reviewer-request builder before freeze.
  auto set_reviewer_request_builder(revision_request_builder builder)
      -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    if (!builder) {
      return wh::core::result<void>::failure(
          wh::core::errc::invalid_argument);
    }
    reviewer_request_builder_ = std::move(builder);
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

  /// Returns the worker role.
  [[nodiscard]] auto worker()
      -> wh::core::result<std::reference_wrapper<agent>> {
    return role_ref(worker_);
  }

  /// Returns the worker role.
  [[nodiscard]] auto worker() const
      -> wh::core::result<std::reference_wrapper<const agent>> {
    return role_ref(worker_);
  }

  /// Returns the optional reviewer role when present.
  [[nodiscard]] auto reviewer()
      -> wh::core::result<std::reference_wrapper<agent>> {
    return role_ref(reviewer_);
  }

  /// Returns the optional reviewer role when present.
  [[nodiscard]] auto reviewer() const
      -> wh::core::result<std::reference_wrapper<const agent>> {
    return role_ref(reviewer_);
  }

  /// Returns the effective reviewer role, falling back to the worker.
  [[nodiscard]] auto effective_reviewer()
      -> wh::core::result<std::reference_wrapper<agent>> {
    if (reviewer_.has_value()) {
      return std::ref(*reviewer_);
    }
    return worker();
  }

  /// Returns the effective reviewer role, falling back to the worker.
  [[nodiscard]] auto effective_reviewer() const
      -> wh::core::result<std::reference_wrapper<const agent>> {
    if (reviewer_.has_value()) {
      return std::cref(*reviewer_);
    }
    return worker();
  }

  /// Returns the configured refinement budget.
  [[nodiscard]] auto max_iterations() const noexcept -> std::size_t {
    return max_iterations_;
  }

  /// Returns the worker-request builder.
  [[nodiscard]] auto worker_request_builder() const noexcept
      -> const revision_request_builder & {
    return worker_request_builder_;
  }

  /// Returns the reviewer-request builder.
  [[nodiscard]] auto reviewer_request_builder() const noexcept
      -> const revision_request_builder & {
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
    if (name_.empty() || !worker_.has_value()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    if (!worker_request_builder_ || !reviewer_request_builder_ ||
        !review_decision_reader_) {
      return wh::core::result<void>::failure(
          wh::core::errc::contract_violation);
    }
    if (!worker_->executable()) {
      return wh::core::result<void>::failure(
          wh::core::errc::contract_violation);
    }
    auto worker_frozen = worker_->freeze();
    if (worker_frozen.has_error()) {
      return worker_frozen;
    }
    if (reviewer_.has_value()) {
      if (!reviewer_->executable()) {
        return wh::core::result<void>::failure(
            wh::core::errc::contract_violation);
      }
      auto reviewer_frozen = reviewer_->freeze();
      if (reviewer_frozen.has_error()) {
        return reviewer_frozen;
      }
    }
    frozen_ = true;
    return {};
  }

  /// Converts this frozen authored shell into the common executable agent surface.
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
  std::optional<agent> worker_{};
  std::optional<agent> reviewer_{};
  std::size_t max_iterations_{3U};
  revision_request_builder worker_request_builder_{nullptr};
  revision_request_builder reviewer_request_builder_{nullptr};
  wh::agent::review_decision_reader review_decision_reader_{nullptr};
  bool frozen_{false};
};

} // namespace wh::agent
