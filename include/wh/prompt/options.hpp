// Defines prompt rendering options, strict missing-variable policy, and
// resolved views used during template expansion.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "wh/core/component.hpp"
#include "wh/prompt/template.hpp"

namespace wh::prompt {

/// Data contract for `prompt_common_options`.
struct prompt_common_options {
  /// Template syntax used during rendering.
  template_syntax syntax{template_syntax::placeholder};
  /// Whether missing variables should fail rendering.
  bool strict_missing_variables{true};
  /// Logical template name used for diagnostics/callback metadata.
  std::string template_name{"default"};
};

/// Borrowed view over resolved prompt options without copying strings.
struct resolved_prompt_options_view {
  /// Effective template syntax.
  template_syntax syntax{template_syntax::placeholder};
  /// Effective strict-missing-variable toggle.
  bool strict_missing_variables{true};
  /// Effective template name.
  std::string_view template_name{"default"};
};

/// Public interface for `prompt_options`.
class prompt_options {
public:
  prompt_options() = default;

  /// Sets baseline options used when no per-call override is provided.
  auto set_base(const prompt_common_options &options) -> prompt_options & {
    base_ = options;
    return *this;
  }

  /// Sets baseline options used when no per-call override is provided.
  auto set_base(prompt_common_options &&options) -> prompt_options & {
    base_ = std::move(options);
    return *this;
  }

  /// Sets per-call option overrides merged on top of the baseline options.
  auto set_call_override(const prompt_common_options &options)
      -> prompt_options & {
    override_ = options;
    return *this;
  }

  /// Sets per-call option overrides merged on top of the baseline options.
  auto set_call_override(prompt_common_options &&options) -> prompt_options & {
    override_ = std::move(options);
    return *this;
  }

  /// Resolves effective options into a borrowed view without deep copies.
  [[nodiscard]] auto resolve_view() const noexcept
      -> resolved_prompt_options_view {
    resolved_prompt_options_view view{};
    view.syntax = base_.syntax;
    view.strict_missing_variables = base_.strict_missing_variables;
    view.template_name = base_.template_name;

    if (!override_.has_value()) {
      return view;
    }

    view.syntax = override_->syntax;
    view.strict_missing_variables = override_->strict_missing_variables;
    if (!override_->template_name.empty()) {
      view.template_name = override_->template_name;
    }
    return view;
  }

  /// Resolves effective options by merging baseline and per-call overrides.
  [[nodiscard]] auto resolve() const -> prompt_common_options {
    if (!override_.has_value()) {
      return base_;
    }
    prompt_common_options resolved = base_;
    resolved.syntax = override_->syntax;
    resolved.strict_missing_variables = override_->strict_missing_variables;
    if (!override_->template_name.empty()) {
      resolved.template_name = override_->template_name;
    }
    return resolved;
  }

  template <typename options_t>
  /// Stores provider-specific extension options for adapter-specific behavior.
  auto set_impl_specific(options_t &&options) -> prompt_options & {
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
  prompt_common_options base_{};
  /// Optional per-call override options layered on top of `base_`.
  std::optional<prompt_common_options> override_{};
  /// Component-level common metadata plus provider-specific extensions.
  wh::core::component_options component_options_{};
};

} // namespace wh::prompt
