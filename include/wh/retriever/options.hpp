// Defines retriever runtime options and scoring/filter controls resolved per
// retrieval call.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "wh/core/component/types.hpp"

namespace wh::retriever {

/// Merge strategy for combining multi-route retrieval results.
enum class recall_merge_policy : std::uint8_t {
  /// Concatenates recall streams in arrival order.
  concat,
  /// Deduplicates recalls by document content.
  dedupe_by_content,
};

/// Data contract for `retriever_common_options`.
struct retriever_common_options {
  /// Maximum number of documents returned.
  std::size_t top_k{4U};
  /// Minimum score threshold for candidate acceptance.
  double score_threshold{0.0};
  /// Filter expression used to include/exclude candidates.
  std::string filter{};
  /// DSL routing key expected on documents.
  std::string dsl{};
  /// Merge policy used for multi-route retrieval results.
  recall_merge_policy merge_policy{recall_merge_policy::concat};
  /// Stop immediately when one route fails.
  bool fail_fast_on_route_error{true};
};

/// Borrowed view over resolved retriever options without copying strings.
struct resolved_retriever_options_view {
  /// Effective top-k limit.
  std::size_t top_k{4U};
  /// Effective score threshold.
  double score_threshold{0.0};
  /// Effective filter expression.
  std::string_view filter{};
  /// Effective DSL route selector.
  std::string_view dsl{};
  /// Effective merge policy.
  recall_merge_policy merge_policy{recall_merge_policy::concat};
  /// Effective route error handling policy.
  bool fail_fast_on_route_error{true};
};

/// Public interface for `retriever_options`.
class retriever_options {
public:
  retriever_options() = default;

  /// Sets baseline options used when no per-call override is provided.
  auto set_base(const retriever_common_options &options) -> retriever_options & {
    base_ = options;
    return *this;
  }

  /// Sets baseline options used when no per-call override is provided.
  auto set_base(retriever_common_options &&options) -> retriever_options & {
    base_ = std::move(options);
    return *this;
  }

  /// Sets per-call option overrides merged on top of the baseline options.
  auto set_call_override(const retriever_common_options &options) -> retriever_options & {
    override_ = options;
    return *this;
  }

  /// Sets per-call option overrides merged on top of the baseline options.
  auto set_call_override(retriever_common_options &&options) -> retriever_options & {
    override_ = std::move(options);
    return *this;
  }

  /// Resolves effective options into a borrowed view without deep copies.
  [[nodiscard]] auto resolve_view() const noexcept -> resolved_retriever_options_view {
    resolved_retriever_options_view view{};
    view.top_k = base_.top_k;
    view.score_threshold = base_.score_threshold;
    view.filter = base_.filter;
    view.dsl = base_.dsl;
    view.merge_policy = base_.merge_policy;
    view.fail_fast_on_route_error = base_.fail_fast_on_route_error;

    if (!override_.has_value()) {
      return view;
    }

    view.top_k = override_->top_k;
    view.score_threshold = override_->score_threshold;
    if (!override_->filter.empty()) {
      view.filter = override_->filter;
    }
    if (!override_->dsl.empty()) {
      view.dsl = override_->dsl;
    }
    view.merge_policy = override_->merge_policy;
    view.fail_fast_on_route_error = override_->fail_fast_on_route_error;
    return view;
  }

  /// Resolves effective options by merging baseline and per-call overrides.
  [[nodiscard]] auto resolve() const -> retriever_common_options {
    if (!override_.has_value()) {
      return base_;
    }
    retriever_common_options resolved = base_;
    resolved.top_k = override_->top_k;
    resolved.score_threshold = override_->score_threshold;
    if (!override_->filter.empty()) {
      resolved.filter = override_->filter;
    }
    if (!override_->dsl.empty()) {
      resolved.dsl = override_->dsl;
    }
    resolved.merge_policy = override_->merge_policy;
    resolved.fail_fast_on_route_error = override_->fail_fast_on_route_error;
    return resolved;
  }

  template <typename options_t>
  /// Stores provider-specific extension options for adapter-specific behavior.
  auto set_impl_specific(options_t &&options) -> retriever_options & {
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
  retriever_common_options base_{};
  /// Optional per-call override options layered on top of `base_`.
  std::optional<retriever_common_options> override_{};
  /// Component-level common metadata plus provider-specific extensions.
  wh::core::component_options component_options_{};
};

} // namespace wh::retriever
