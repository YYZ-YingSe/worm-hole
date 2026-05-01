// Defines embedding runtime options and per-call override resolution for
// embedding component execution.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "wh/core/component/types.hpp"

namespace wh::embedding {

/// Policy applied when one item in an embedding batch fails.
enum class batch_failure_policy : std::uint8_t {
  /// Aborts the batch when any single item fails.
  fail_fast,
  /// Returns successful vectors while surfacing failed items.
  partial_success,
};

/// Data contract for `embedding_common_options`.
struct embedding_common_options {
  /// Provider/model identifier to route embedding request.
  std::string model_id{};
  /// Failure handling policy for batch embedding path.
  batch_failure_policy failure_policy{batch_failure_policy::fail_fast};
};

/// Borrowed view over resolved embedding options without copying strings.
struct resolved_embedding_options_view {
  /// Effective embedding model id.
  std::string_view model_id{};
  /// Effective batch failure policy.
  batch_failure_policy failure_policy{batch_failure_policy::fail_fast};
};

/// Public interface for `embedding_options`.
class embedding_options {
public:
  embedding_options() = default;

  /// Sets baseline options used when no per-call override is provided.
  auto set_base(const embedding_common_options &options) -> embedding_options & {
    base_ = options;
    return *this;
  }

  /// Sets baseline options used when no per-call override is provided.
  auto set_base(embedding_common_options &&options) -> embedding_options & {
    base_ = std::move(options);
    return *this;
  }

  /// Sets per-call option overrides merged on top of the baseline options.
  auto set_call_override(const embedding_common_options &options) -> embedding_options & {
    override_ = options;
    return *this;
  }

  /// Sets per-call option overrides merged on top of the baseline options.
  auto set_call_override(embedding_common_options &&options) -> embedding_options & {
    override_ = std::move(options);
    return *this;
  }

  /// Resolves effective options into a borrowed view without deep copies.
  [[nodiscard]] auto resolve_view() const noexcept -> resolved_embedding_options_view {
    resolved_embedding_options_view view{};
    view.model_id = base_.model_id;
    view.failure_policy = base_.failure_policy;

    if (!override_.has_value()) {
      return view;
    }

    if (!override_->model_id.empty()) {
      view.model_id = override_->model_id;
    }
    view.failure_policy = override_->failure_policy;
    return view;
  }

  /// Resolves effective options by merging baseline and per-call overrides.
  [[nodiscard]] auto resolve() const -> embedding_common_options {
    if (!override_.has_value()) {
      return base_;
    }
    embedding_common_options resolved = base_;
    if (!override_->model_id.empty()) {
      resolved.model_id = override_->model_id;
    }
    resolved.failure_policy = override_->failure_policy;
    return resolved;
  }

  template <typename options_t>
  /// Stores provider-specific extension options for adapter-specific behavior.
  auto set_impl_specific(options_t &&options) -> embedding_options & {
    component_options_.set_impl_specific(std::forward<options_t>(options));
    return *this;
  }

  template <typename options_t>
  /// Returns provider-specific options when the stored type matches
  /// `options_t`.
  [[nodiscard]] auto impl_specific_if() const -> const options_t * {
    return component_options_.impl_specific_if<options_t>();
  }

  /// Returns component-level common metadata plus provider-specific extensions.
  [[nodiscard]] auto component_options() noexcept -> wh::core::component_options & {
    return component_options_;
  }

  /// Returns component-level common metadata plus provider-specific extensions.
  [[nodiscard]] auto component_options() const noexcept -> const wh::core::component_options & {
    return component_options_;
  }

private:
  /// Baseline options shared by all calls.
  embedding_common_options base_{};
  /// Optional per-call overrides layered on top of `base_`.
  std::optional<embedding_common_options> override_{};
  /// Component-level common metadata plus provider-specific extensions.
  wh::core::component_options component_options_{};
};

} // namespace wh::embedding
