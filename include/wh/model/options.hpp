// Defines chat model runtime options, per-call overrides, and resolved views
// for fallback policies, tool choice, and structured output.
#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wh/core/component.hpp"
#include "wh/schema/tool.hpp"

namespace wh::model {

/// Strategy for ordering candidate models during fallback selection.
enum class model_selection_policy : std::uint8_t {
  /// Keeps quality-prioritized ordering from configured candidates.
  quality_first,
  /// Reorders candidates to prefer lower-cost models first.
  cost_first,
  /// Reorders candidates to prefer lower-latency models first.
  latency_first,
};

/// Preference order for structured-output implementation paths.
enum class structured_output_preference : std::uint8_t {
  /// Tries provider-native structured output before tool-call fallback.
  provider_native_first,
  /// Tries tool-call structured output before provider-native fallback.
  tool_call_first,
};

/// Data contract for `model_fallback_policy`.
struct model_fallback_policy {
  /// Explicit model candidate order appended after primary model.
  std::vector<std::string> ordered_candidates{};
  /// Whether to preserve per-attempt failure reasons in report.
  bool keep_failure_reasons{true};
};

/// Data contract for `structured_output_policy`.
struct structured_output_policy {
  /// Preference order between provider-native and tool-call output modes.
  structured_output_preference preference{structured_output_preference::provider_native_first};
  /// Allows tool-call fallback when provider-native path is unavailable.
  bool allow_tool_fallback{true};
};

/// Data contract for `structured_output_plan`.
struct structured_output_plan {
  /// True when provider-native structured output is selected.
  bool use_provider_native{true};
  /// True when tool-call fallback structured output is selected.
  bool use_tool_call_fallback{false};
};

/// Data contract for `chat_model_common_options`.
struct chat_model_common_options {
  /// Preferred primary model identifier.
  std::string model_id{};
  /// Sampling temperature.
  double temperature{0.7};
  /// Nucleus sampling top-p value.
  double top_p{1.0};
  /// Maximum completion token count.
  std::size_t max_tokens{1024U};
  /// Optional stop token list for completion truncation.
  std::vector<std::string> stop_tokens{};
  /// Candidate ordering strategy for fallback model selection.
  model_selection_policy selection_policy{model_selection_policy::quality_first};
  /// Fallback behavior configuration.
  model_fallback_policy fallback{};
  /// Tool-selection policy used by model execution.
  wh::schema::tool_choice tool_choice{};
  /// Optional whitelist of tool names allowed for this request.
  std::vector<std::string> allowed_tool_names{};
  /// Structured-output negotiation policy.
  structured_output_policy structured_output{};
};

/// Borrowed view over resolved model options without container/string copies.
struct resolved_chat_model_options_view {
  /// Effective primary model identifier.
  std::string_view model_id{};
  /// Effective sampling temperature.
  double temperature{0.7};
  /// Effective top-p sampling value.
  double top_p{1.0};
  /// Effective maximum token limit.
  std::size_t max_tokens{1024U};
  /// Effective stop-token list.
  std::span<const std::string> stop_tokens{};
  /// Effective model-selection policy.
  model_selection_policy selection_policy{model_selection_policy::quality_first};
  /// Effective fallback policy storage.
  const model_fallback_policy *fallback{nullptr};
  /// Effective tool-choice policy storage.
  const wh::schema::tool_choice *tool_choice{nullptr};
  /// Effective tool-name whitelist.
  std::span<const std::string> allowed_tool_names{};
  /// Effective structured-output policy storage.
  const structured_output_policy *structured_output{nullptr};

  /// Returns resolved fallback policy by reference.
  [[nodiscard]] auto fallback_ref() const noexcept -> const model_fallback_policy & {
    return *fallback;
  }

  /// Returns resolved tool-choice policy by reference.
  [[nodiscard]] auto tool_choice_ref() const noexcept -> const wh::schema::tool_choice & {
    return *tool_choice;
  }

  /// Returns resolved structured-output policy by reference.
  [[nodiscard]] auto structured_output_ref() const noexcept -> const structured_output_policy & {
    return *structured_output;
  }
};

/// Public interface for `chat_model_options`.
class chat_model_options {
public:
  chat_model_options() = default;

  /// Sets baseline options used when no per-call override is provided.
  auto set_base(const chat_model_common_options &options) -> chat_model_options & {
    base_ = options;
    return *this;
  }

  /// Sets baseline options used when no per-call override is provided.
  auto set_base(chat_model_common_options &&options) -> chat_model_options & {
    base_ = std::move(options);
    return *this;
  }

  /// Sets per-call option overrides merged on top of the baseline options.
  auto set_call_override(const chat_model_common_options &options) -> chat_model_options & {
    override_ = options;
    return *this;
  }

  /// Sets per-call option overrides merged on top of the baseline options.
  auto set_call_override(chat_model_common_options &&options) -> chat_model_options & {
    override_ = std::move(options);
    return *this;
  }

  /// Returns the stored baseline option set.
  [[nodiscard]] auto base() const noexcept -> const chat_model_common_options & { return base_; }

  /// Returns the optional per-call override option set.
  [[nodiscard]] auto call_override() const noexcept
      -> const std::optional<chat_model_common_options> & {
    return override_;
  }

  /// Resolves effective options into a borrowed view without deep copies.
  [[nodiscard]] auto resolve_view() const noexcept -> resolved_chat_model_options_view {
    resolved_chat_model_options_view view{};
    view.model_id = base_.model_id;
    view.temperature = base_.temperature;
    view.top_p = base_.top_p;
    view.max_tokens = base_.max_tokens;
    view.stop_tokens = base_.stop_tokens;
    view.selection_policy = base_.selection_policy;
    view.fallback = &base_.fallback;
    view.tool_choice = &base_.tool_choice;
    view.allowed_tool_names = base_.allowed_tool_names;
    view.structured_output = &base_.structured_output;

    if (!override_.has_value()) {
      return view;
    }

    if (!override_->model_id.empty()) {
      view.model_id = override_->model_id;
    }
    view.temperature = override_->temperature;
    view.top_p = override_->top_p;
    view.max_tokens = override_->max_tokens;
    if (!override_->stop_tokens.empty()) {
      view.stop_tokens = override_->stop_tokens;
    }
    view.selection_policy = override_->selection_policy;
    if (!override_->fallback.ordered_candidates.empty()) {
      view.fallback = &override_->fallback;
    }
    view.tool_choice = &override_->tool_choice;
    if (!override_->allowed_tool_names.empty()) {
      view.allowed_tool_names = override_->allowed_tool_names;
    }
    view.structured_output = &override_->structured_output;
    return view;
  }

  /// Resolves effective options by merging baseline and per-call overrides.
  [[nodiscard]] auto resolve() const -> chat_model_common_options {
    if (!override_.has_value()) {
      return base_;
    }

    chat_model_common_options resolved = base_;
    if (!override_->model_id.empty()) {
      resolved.model_id = override_->model_id;
    }
    resolved.temperature = override_->temperature;
    resolved.top_p = override_->top_p;
    resolved.max_tokens = override_->max_tokens;
    if (!override_->stop_tokens.empty()) {
      resolved.stop_tokens = override_->stop_tokens;
    }
    resolved.selection_policy = override_->selection_policy;
    if (!override_->fallback.ordered_candidates.empty()) {
      resolved.fallback = override_->fallback;
    }
    resolved.tool_choice = override_->tool_choice;
    if (!override_->allowed_tool_names.empty()) {
      resolved.allowed_tool_names = override_->allowed_tool_names;
    }
    resolved.structured_output = override_->structured_output;
    return resolved;
  }

  template <typename options_t>
  /// Stores provider-specific extension options for adapter-specific behavior.
  auto set_impl_specific(options_t &&options) -> chat_model_options & {
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
  chat_model_common_options base_{};
  /// Optional per-call override options layered on top of `base_`.
  std::optional<chat_model_common_options> override_{};
  /// Component-level common metadata plus provider-specific extensions.
  wh::core::component_options component_options_{};
};

/// Freezes fallback model candidate ordering using explicit and discovered
/// candidates.
[[nodiscard]] inline auto
freeze_model_candidates(const chat_model_common_options &options,
                        std::span<const std::string> discovered_candidates)
    -> std::vector<std::string> {
  std::vector<std::string> ordered{};
  ordered.reserve(1U + discovered_candidates.size() + options.fallback.ordered_candidates.size());

  if (!options.model_id.empty()) {
    ordered.push_back(options.model_id);
  }
  for (const auto &candidate : discovered_candidates) {
    if (std::ranges::find(ordered, candidate) == ordered.end()) {
      ordered.push_back(candidate);
    }
  }
  for (const auto &candidate : options.fallback.ordered_candidates) {
    if (std::ranges::find(ordered, candidate) == ordered.end()) {
      ordered.push_back(candidate);
    }
  }

  if (ordered.size() < 2U) {
    return ordered;
  }

  if (options.selection_policy == model_selection_policy::quality_first) {
    return ordered;
  }
  if (options.selection_policy == model_selection_policy::cost_first) {
    std::ranges::reverse(ordered);
    return ordered;
  }
  std::rotate(ordered.begin(), ordered.begin() + 1, ordered.end());
  return ordered;
}

/// Freezes fallback model candidate ordering from a resolved options view.
[[nodiscard]] inline auto
freeze_model_candidates(const resolved_chat_model_options_view &options,
                        std::span<const std::string> discovered_candidates)
    -> std::vector<std::string> {
  std::vector<std::string> ordered{};
  ordered.reserve(1U + discovered_candidates.size() +
                  options.fallback_ref().ordered_candidates.size());

  if (!options.model_id.empty()) {
    ordered.emplace_back(options.model_id);
  }
  for (const auto &candidate : discovered_candidates) {
    if (std::ranges::find(ordered, candidate) == ordered.end()) {
      ordered.push_back(candidate);
    }
  }
  for (const auto &candidate : options.fallback_ref().ordered_candidates) {
    if (std::ranges::find(ordered, candidate) == ordered.end()) {
      ordered.push_back(candidate);
    }
  }

  if (ordered.size() < 2U) {
    return ordered;
  }

  if (options.selection_policy == model_selection_policy::quality_first) {
    return ordered;
  }
  if (options.selection_policy == model_selection_policy::cost_first) {
    std::ranges::reverse(ordered);
    return ordered;
  }
  std::rotate(ordered.begin(), ordered.begin() + 1, ordered.end());
  return ordered;
}

/// Negotiates the effective structured-output mode from policy and runtime
/// capabilities.
[[nodiscard]] inline auto negotiate_structured_output(const structured_output_policy &policy,
                                                      const bool provider_native_supported,
                                                      const bool tool_call_supported)
    -> structured_output_plan {
  if (policy.preference == structured_output_preference::provider_native_first) {
    if (provider_native_supported) {
      return structured_output_plan{true, false};
    }
    if (policy.allow_tool_fallback && tool_call_supported) {
      return structured_output_plan{false, true};
    }
    return structured_output_plan{false, false};
  }

  if (tool_call_supported) {
    return structured_output_plan{false, true};
  }
  if (provider_native_supported) {
    return structured_output_plan{true, false};
  }
  return structured_output_plan{false, false};
}

/// Negotiates structured-output mode from a resolved options view.
[[nodiscard]] inline auto
negotiate_structured_output(const resolved_chat_model_options_view &options,
                            const bool provider_native_supported, const bool tool_call_supported)
    -> structured_output_plan {
  return negotiate_structured_output(options.structured_output_ref(), provider_native_supported,
                                     tool_call_supported);
}

} // namespace wh::model
