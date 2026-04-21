#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "wh/core/any.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"

namespace wh::core {

/// High-level component categories in the pipeline graph.
enum class component_kind : std::uint8_t {
  /// Chat/model inference component.
  model,
  /// Prompt/template rendering component.
  prompt,
  /// Tool execution component.
  tool,
  /// Retrieval component.
  retriever,
  /// Embedding/vectorization component.
  embedding,
  /// Index writing/querying component.
  indexer,
  /// Document parsing component.
  document,
  /// User-defined custom component.
  custom,
};

/// Static metadata describing a component implementation.
struct component_descriptor {
  /// Stable logical type name for diagnostics/registration.
  std::string type_name{};
  /// High-level component category.
  component_kind kind{component_kind::custom};
};

/// Baseline options shared by all component kinds.
struct component_common_options {
  /// Enables callback emission for this call.
  bool callbacks_enabled{true};
  /// Distributed trace id.
  std::string trace_id{};
  /// Distributed span id.
  std::string span_id{};
};

/// Optional per-call overlay fields for component options.
struct component_override_options {
  /// Optional callback override; absent keeps base value.
  std::optional<bool> callbacks_enabled{};
  /// Optional trace id override; absent keeps base value.
  std::optional<std::string> trace_id{};
  /// Optional span id override; absent keeps base value.
  std::optional<std::string> span_id{};
};

/// Borrowed view over resolved component options without string copies.
struct resolved_component_options_view {
  /// Effective callback emission flag for this call.
  bool callbacks_enabled{true};
  /// Effective trace id.
  std::string_view trace_id{};
  /// Effective span id.
  std::string_view span_id{};
};

/// Storage for base options, per-call overrides, and implementation extras.
class component_options {
public:
  component_options() = default;

  /// Sets base options applied to all invocations.
  auto set_base(const component_common_options &options) -> component_options & {
    base_ = options;
    return *this;
  }

  /// Sets base options applied to all invocations.
  auto set_base(component_common_options &&options) -> component_options & {
    base_ = std::move(options);
    return *this;
  }

  /// Sets one-shot override options with explicit optional field semantics.
  auto set_call_override(const component_override_options &options) -> component_options & {
    override_ = options;
    return *this;
  }

  /// Sets one-shot override options with explicit optional field semantics.
  auto set_call_override(component_override_options &&options) -> component_options & {
    override_ = std::move(options);
    return *this;
  }

  /// Clears one-shot call overrides.
  auto clear_call_override() -> component_options & {
    override_.reset();
    return *this;
  }

  /// Returns base options.
  [[nodiscard]] auto base() const noexcept -> const component_common_options & { return base_; }

  /// Returns optional per-call override options.
  [[nodiscard]] auto call_override() const noexcept
      -> const std::optional<component_override_options> & {
    return override_;
  }

  /// Returns a borrowed view of effective options without string copies.
  [[nodiscard]] auto resolve_view() const noexcept -> resolved_component_options_view {
    resolved_component_options_view view{};
    view.callbacks_enabled = base_.callbacks_enabled;
    view.trace_id = base_.trace_id;
    view.span_id = base_.span_id;
    if (!override_.has_value()) {
      return view;
    }

    if (override_->callbacks_enabled.has_value()) {
      view.callbacks_enabled = *override_->callbacks_enabled;
    }
    if (override_->trace_id.has_value()) {
      view.trace_id = *override_->trace_id;
    }
    if (override_->span_id.has_value()) {
      view.span_id = *override_->span_id;
    }
    return view;
  }

  /// Resolves effective options by overlaying call override on base.
  [[nodiscard]] auto resolve() const -> component_common_options {
    component_common_options resolved = base_;
    if (!override_.has_value()) {
      return resolved;
    }

    if (override_->callbacks_enabled.has_value()) {
      resolved.callbacks_enabled = *override_->callbacks_enabled;
    }
    if (override_->trace_id.has_value()) {
      resolved.trace_id = *override_->trace_id;
    }
    if (override_->span_id.has_value()) {
      resolved.span_id = *override_->span_id;
    }
    return resolved;
  }

  /// Stores implementation-specific options by concrete type.
  template <typename options_t> auto set_impl_specific(options_t &&options) -> component_options & {
    using stored_t = std::remove_cvref_t<options_t>;
    const auto key = wh::core::any_type_key_v<stored_t>;
    auto iter = impl_specific_.find(key);
    if (iter == impl_specific_.end()) {
      impl_specific_.emplace(
          key, wh::core::any{std::in_place_type<stored_t>, std::forward<options_t>(options)});
      return *this;
    }
    iter->second.template emplace<stored_t>(std::forward<options_t>(options));
    return *this;
  }

  /// Returns typed implementation-specific options pointer if present.
  template <typename options_t> [[nodiscard]] auto impl_specific_if() const -> const options_t * {
    const auto iter = impl_specific_.find(wh::core::any_type_key_v<options_t>);
    if (iter == impl_specific_.end()) {
      return nullptr;
    }
    return wh::core::any_cast<options_t>(&iter->second);
  }

  /// Returns typed implementation-specific options as result reference.
  template <typename options_t>
  [[nodiscard]] auto impl_specific_as() const -> result<std::reference_wrapper<const options_t>> {
    const auto *typed = impl_specific_if<options_t>();
    if (typed == nullptr) {
      return result<std::reference_wrapper<const options_t>>::failure(errc::not_found);
    }
    return std::cref(*typed);
  }

private:
  /// Default options applied to all calls unless overridden.
  component_common_options base_{};
  /// Per-call temporary overrides.
  std::optional<component_override_options> override_{};
  /// Type-indexed implementation-specific option payloads.
  std::unordered_map<wh::core::any_type_key, wh::core::any, wh::core::any_type_key_hash>
      impl_specific_{};
};

} // namespace wh::core
