// Defines indexer runtime options and resolved option views for indexing
// operations.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "wh/core/component.hpp"

namespace wh::indexer {

/// Policy applied when a document write fails.
enum class write_failure_policy : std::uint8_t {
  /// Retries failed writes before giving up.
  retry,
  /// Skips failed documents and continues remaining writes.
  skip,
  /// Stops the whole write batch on first failure.
  stop,
};

/// Data contract for `indexer_common_options`.
struct indexer_common_options {
  /// Failure policy for write attempts.
  write_failure_policy failure_policy{write_failure_policy::stop};
  /// Retry count when `failure_policy == retry`.
  std::size_t max_retries{0U};
  /// Optional logical sub-index name.
  std::string sub_index{};
  /// Embedding model name used by combined indexing.
  std::string embedding_model{};
  /// Enables embedding+document combined indexing behavior.
  bool combine_with_embedding{false};
};

/// Borrowed view over resolved indexer options without copying strings.
struct resolved_indexer_options_view {
  /// Effective write failure policy.
  write_failure_policy failure_policy{write_failure_policy::stop};
  /// Effective retry budget.
  std::size_t max_retries{0U};
  /// Effective sub-index name.
  std::string_view sub_index{};
  /// Effective embedding model hint.
  std::string_view embedding_model{};
  /// Effective combined-indexing toggle.
  bool combine_with_embedding{false};
};

/// Public interface for `indexer_options`.
class indexer_options {
public:
  indexer_options() = default;

  /// Sets baseline options used when no per-call override is provided.
  auto set_base(const indexer_common_options &options) -> indexer_options & {
    base_ = options;
    return *this;
  }

  /// Sets baseline options used when no per-call override is provided.
  auto set_base(indexer_common_options &&options) -> indexer_options & {
    base_ = std::move(options);
    return *this;
  }

  /// Sets per-call option overrides merged on top of the baseline options.
  auto set_call_override(const indexer_common_options &options) -> indexer_options & {
    override_ = options;
    return *this;
  }

  /// Sets per-call option overrides merged on top of the baseline options.
  auto set_call_override(indexer_common_options &&options) -> indexer_options & {
    override_ = std::move(options);
    return *this;
  }

  /// Resolves effective options into a borrowed view without deep copies.
  [[nodiscard]] auto resolve_view() const noexcept -> resolved_indexer_options_view {
    resolved_indexer_options_view view{};
    view.failure_policy = base_.failure_policy;
    view.max_retries = base_.max_retries;
    view.sub_index = base_.sub_index;
    view.embedding_model = base_.embedding_model;
    view.combine_with_embedding = base_.combine_with_embedding;

    if (!override_.has_value()) {
      return view;
    }

    view.failure_policy = override_->failure_policy;
    view.max_retries = override_->max_retries;
    if (!override_->sub_index.empty()) {
      view.sub_index = override_->sub_index;
    }
    if (!override_->embedding_model.empty()) {
      view.embedding_model = override_->embedding_model;
    }
    view.combine_with_embedding = override_->combine_with_embedding;
    return view;
  }

  /// Resolves effective options by merging baseline and per-call overrides.
  [[nodiscard]] auto resolve() const -> indexer_common_options {
    if (!override_.has_value()) {
      return base_;
    }
    indexer_common_options resolved = base_;
    resolved.failure_policy = override_->failure_policy;
    resolved.max_retries = override_->max_retries;
    if (!override_->sub_index.empty()) {
      resolved.sub_index = override_->sub_index;
    }
    if (!override_->embedding_model.empty()) {
      resolved.embedding_model = override_->embedding_model;
    }
    resolved.combine_with_embedding = override_->combine_with_embedding;
    return resolved;
  }

  template <typename options_t>
  /// Stores provider-specific extension options for adapter-specific behavior.
  auto set_impl_specific(options_t &&options) -> indexer_options & {
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
  indexer_common_options base_{};
  /// Optional per-call override options layered on top of `base_`.
  std::optional<indexer_common_options> override_{};
  /// Component-level common metadata plus provider-specific extensions.
  wh::core::component_options component_options_{};
};

} // namespace wh::indexer
