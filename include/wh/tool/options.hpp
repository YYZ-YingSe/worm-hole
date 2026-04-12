// Defines tool runtime options and per-call override resolution.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "wh/core/component.hpp"

namespace wh::tool {

/// Failure handling policy for tool execution.
enum class tool_failure_policy : std::uint8_t {
  /// Stops execution on the first tool failure.
  fail_fast,
  /// Skips failed calls and continues the pipeline.
  skip,
  /// Retries failed calls up to the configured retry budget.
  retry,
};

/// Common tool execution options.
struct tool_common_options {
  /// Failure strategy for tool execution.
  tool_failure_policy failure_policy{tool_failure_policy::fail_fast};
  /// Retry count when failure policy is `retry`.
  std::size_t max_retries{0U};
  /// Optional timeout/deadline label copied into callback context.
  std::string timeout_label{};
};

/// Borrowed view over resolved tool options without copying strings.
struct resolved_tool_options_view {
  /// Effective failure strategy.
  tool_failure_policy failure_policy{tool_failure_policy::fail_fast};
  /// Effective retry limit.
  std::size_t max_retries{0U};
  /// Effective timeout label.
  std::string_view timeout_label{};
};

/// Layered tool options: base + per-call override + impl-specific options.
class tool_options {
public:
  tool_options() = default;

  /// Sets base options.
  auto set_base(const tool_common_options &options) -> tool_options & {
    base_ = options;
    return *this;
  }

  /// Sets base options.
  auto set_base(tool_common_options &&options) -> tool_options & {
    base_ = std::move(options);
    return *this;
  }

  /// Sets per-call override options.
  auto set_call_override(const tool_common_options &options) -> tool_options & {
    override_ = options;
    return *this;
  }

  /// Sets per-call override options.
  auto set_call_override(tool_common_options &&options) -> tool_options & {
    override_ = std::move(options);
    return *this;
  }

  /// Returns baseline tool options.
  [[nodiscard]] auto base() const noexcept -> const tool_common_options & {
    return base_;
  }

  /// Returns optional per-call override options.
  [[nodiscard]] auto call_override() const noexcept
      -> const std::optional<tool_common_options> & {
    return override_;
  }

  /// Resolves effective options into a borrowed view without deep copies.
  [[nodiscard]] auto resolve_view() const noexcept
      -> resolved_tool_options_view {
    resolved_tool_options_view view{};
    view.failure_policy = base_.failure_policy;
    view.max_retries = base_.max_retries;
    view.timeout_label = base_.timeout_label;

    if (!override_.has_value()) {
      return view;
    }

    view.failure_policy = override_->failure_policy;
    view.max_retries = override_->max_retries;
    if (!override_->timeout_label.empty()) {
      view.timeout_label = override_->timeout_label;
    }
    return view;
  }

  /// Resolves effective options by overlaying override on base.
  [[nodiscard]] auto resolve() const -> tool_common_options {
    if (!override_.has_value()) {
      return base_;
    }
    tool_common_options resolved = base_;
    resolved.failure_policy = override_->failure_policy;
    resolved.max_retries = override_->max_retries;
    if (!override_->timeout_label.empty()) {
      resolved.timeout_label = override_->timeout_label;
    }
    return resolved;
  }

  template <typename options_t>
  /// Stores implementation-specific options payload.
  auto set_impl_specific(options_t &&options) -> tool_options & {
    component_options_.set_impl_specific(std::forward<options_t>(options));
    return *this;
  }

  template <typename options_t>
  /// Returns implementation-specific options pointer when present.
  [[nodiscard]] auto impl_specific_if() const -> const options_t * {
    return component_options_.impl_specific_if<options_t>();
  }

  /// Returns component-level common metadata plus provider-specific extensions.
  [[nodiscard]] auto component_options() noexcept
      -> wh::core::component_options & {
    return component_options_;
  }

  /// Returns component-level common metadata plus provider-specific extensions.
  [[nodiscard]] auto component_options() const noexcept
      -> const wh::core::component_options & {
    return component_options_;
  }

private:
  /// Baseline options shared by all calls.
  tool_common_options base_{};
  /// Optional per-call override options layered on top of `base_`.
  std::optional<tool_common_options> override_{};
  /// Component-level common metadata plus provider-specific extensions.
  wh::core::component_options component_options_{};
};

} // namespace wh::tool
