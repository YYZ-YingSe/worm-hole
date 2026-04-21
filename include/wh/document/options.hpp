// Defines document pipeline options for loader/transformer/parser stages with
// per-stage override controls.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "wh/core/component.hpp"
#include "wh/document/parser/option.hpp"

namespace wh::document {

/// Data contract for `parser_options`.
struct parser_options {
  /// Source URI/path used by parser stage.
  std::string uri{};
  /// Metadata key/value pairs attached to parsed documents.
  parser::parser_string_map extra_meta{};
  /// Parser-format specific options passed to concrete parser implementation.
  parser::parser_string_map format_options{};
};

/// Data contract for `loader_common_options`.
struct loader_common_options {
  /// Parser-stage options propagated into parse request.
  parser_options parser{};
};

/// Borrowed view over resolved loader options.
struct resolved_loader_options_view {
  using parser_map_t = parser::parser_string_map;

  /// Effective parser options baseline.
  const parser_options *base_parser{nullptr};
  /// Optional parser override options.
  const parser_options *override_parser{nullptr};

  /// Returns resolved parser URI.
  [[nodiscard]] auto parser_uri() const -> std::string_view {
    if (override_parser != nullptr && !override_parser->uri.empty()) {
      return override_parser->uri;
    }
    return base_parser->uri;
  }

  /// Materializes merged parser extra metadata map.
  [[nodiscard]] auto materialize_parser_extra_meta() const -> parser_map_t {
    parser_map_t merged = base_parser->extra_meta;
    if (override_parser == nullptr) {
      return merged;
    }
    for (const auto &[key, value] : override_parser->extra_meta) {
      merged.insert_or_assign(key, value);
    }
    return merged;
  }

  /// Materializes merged parser format-options map.
  [[nodiscard]] auto materialize_parser_format_options() const -> parser_map_t {
    parser_map_t merged = base_parser->format_options;
    if (override_parser == nullptr) {
      return merged;
    }
    for (const auto &[key, value] : override_parser->format_options) {
      merged.insert_or_assign(key, value);
    }
    return merged;
  }
};

/// Public interface for `loader_options`.
class loader_options {
public:
  loader_options() = default;

  /// Sets baseline options used when no per-call override is provided.
  auto set_base(const loader_common_options &options) -> loader_options & {
    base_ = options;
    return *this;
  }

  /// Sets baseline options used when no per-call override is provided.
  auto set_base(loader_common_options &&options) -> loader_options & {
    base_ = std::move(options);
    return *this;
  }

  /// Sets per-call option overrides merged on top of the baseline options.
  auto set_call_override(const loader_common_options &options) -> loader_options & {
    override_ = options;
    return *this;
  }

  /// Sets per-call option overrides merged on top of the baseline options.
  auto set_call_override(loader_common_options &&options) -> loader_options & {
    override_ = std::move(options);
    return *this;
  }

  /// Resolves effective options into a borrowed view.
  [[nodiscard]] auto resolve_view() const noexcept -> resolved_loader_options_view {
    resolved_loader_options_view view{};
    view.base_parser = &base_.parser;
    view.override_parser = nullptr;

    if (!override_.has_value()) {
      return view;
    }

    view.override_parser = &override_->parser;
    return view;
  }

  /// Resolves effective options by merging baseline and per-call overrides.
  [[nodiscard]] auto resolve() const -> loader_common_options {
    if (!override_.has_value()) {
      return base_;
    }

    loader_common_options resolved = base_;
    if (!override_->parser.uri.empty()) {
      resolved.parser.uri = override_->parser.uri;
    }
    for (const auto &[key, value] : override_->parser.extra_meta) {
      resolved.parser.extra_meta.insert_or_assign(key, value);
    }
    for (const auto &[key, value] : override_->parser.format_options) {
      resolved.parser.format_options.insert_or_assign(key, value);
    }
    return resolved;
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
  loader_common_options base_{};
  /// Optional per-call override options layered on top of `base_`.
  std::optional<loader_common_options> override_{};
  /// Component-level common metadata plus provider-specific extensions.
  wh::core::component_options component_options_{};
};

} // namespace wh::document
