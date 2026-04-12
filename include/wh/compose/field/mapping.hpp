// Defines workflow field-mapping contracts used by compile and runtime mapping.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wh/compose/types.hpp"
#include "wh/core/function.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"

namespace wh::compose {

/// Missing-key strategy for source path extraction.
enum class field_missing_policy : std::uint8_t {
  /// Missing source key is treated as hard failure.
  fail = 0U,
  /// Missing source key skips current mapping rule.
  skip,
};

/// Workflow edge semantics for scheduling and data propagation.
enum class workflow_dependency_kind : std::uint8_t {
  /// Dependency contributes control-order constraints only.
  control = 0U,
  /// Dependency contributes data propagation/mapping only.
  data,
  /// Dependency contributes both control order and data propagation.
  control_data,
};

/// One field mapping rule from source path to target path.
struct field_mapping_rule {
  /// Custom extractor callback type used by mapping rules.
  using extractor_callback =
      wh::core::callback_function<wh::core::result<graph_value>(
          const graph_value_map &, wh::core::run_context &) const>;

  /// Source path expression, for example `order.user.id`.
  std::string from_path{};
  /// Target path expression, for example `input.user_id`.
  std::string to_path{};
  /// Missing source key strategy.
  field_missing_policy missing_policy{field_missing_policy::fail};
  /// Optional static value injected directly to target path.
  std::optional<graph_value> static_value{};
  /// Optional custom extractor that overrides `from_path` reading.
  extractor_callback extractor{nullptr};
};

/// Workflow dependency descriptor combining control and data mapping semantics.
struct workflow_dependency {
  /// Upstream node key.
  std::string from{};
  /// Downstream node key.
  std::string to{};
  /// Combined dependency semantics.
  workflow_dependency_kind kind{workflow_dependency_kind::control_data};
  /// Field mapping rules attached to this dependency.
  std::vector<field_mapping_rule> mappings{};
};

/// Encoded field path segments produced by path parser.
struct field_path {
  /// Original path text for diagnostics.
  std::string text{};
  /// Parsed ordered path segments.
  std::vector<std::string> segments{};
};

/// One precompiled field mapping rule with parsed field paths.
struct compiled_field_mapping_rule {
  /// Parsed source path for path-based extraction.
  std::optional<field_path> from_path{};
  /// Parsed target path for write-back.
  field_path to_path{};
  /// Missing source key strategy.
  field_missing_policy missing_policy{field_missing_policy::fail};
  /// Optional static value injected directly to target path.
  std::optional<graph_value> static_value{};
  /// Optional custom extractor that overrides path extraction.
  field_mapping_rule::extractor_callback extractor{nullptr};
};

/// Parses one dot-separated field path string.
[[nodiscard]] inline auto parse_field_path(const std::string_view text)
    -> wh::core::result<field_path> {
  if (text.empty()) {
    return wh::core::result<field_path>::failure(
        wh::core::errc::invalid_argument);
  }

  field_path parsed{};
  parsed.text = std::string{text};
  std::size_t begin = 0U;
  while (begin <= text.size()) {
    const auto end = text.find('.', begin);
    const auto stop = end == std::string_view::npos ? text.size() : end;
    const auto segment = text.substr(begin, stop - begin);
    if (segment.empty() || segment.find('\x1F') != std::string_view::npos) {
      return wh::core::result<field_path>::failure(
          wh::core::errc::invalid_argument);
    }
    parsed.segments.emplace_back(segment);
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1U;
  }
  return parsed;
}

/// Compiles one mapping rule into parsed-path representation.
[[nodiscard]] inline auto
compile_field_mapping_rule(const field_mapping_rule &rule)
    -> wh::core::result<compiled_field_mapping_rule> {
  auto parsed_to_path = parse_field_path(rule.to_path);
  if (parsed_to_path.has_error()) {
    return wh::core::result<compiled_field_mapping_rule>::failure(
        parsed_to_path.error());
  }

  std::optional<field_path> parsed_from_path{};
  if (!rule.static_value.has_value() && !rule.extractor) {
    auto source_path = parse_field_path(rule.from_path);
    if (source_path.has_error()) {
      return wh::core::result<compiled_field_mapping_rule>::failure(
          source_path.error());
    }
    parsed_from_path = std::move(source_path).value();
  }

  return compiled_field_mapping_rule{
      .from_path = std::move(parsed_from_path),
      .to_path = std::move(parsed_to_path).value(),
      .missing_policy = rule.missing_policy,
      .static_value = rule.static_value,
      .extractor = rule.extractor,
  };
}

} // namespace wh::compose
