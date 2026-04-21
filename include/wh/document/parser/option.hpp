// Defines parser-specific options and resolved option views used during
// document parse execution and callback lifecycle handling.
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "wh/core/type_traits.hpp"

namespace wh::document::parser {

/// Shared key/value map type used by parser options.
using parser_string_map =
    std::unordered_map<std::string, std::string, wh::core::transparent_string_hash,
                       wh::core::transparent_string_equal>;

/// Data contract for `parse_options`.
struct parse_options {
  /// Source URI/path associated with content.
  std::string uri{};
  /// Metadata entries to attach to parsed documents.
  parser_string_map extra_meta{};
  /// Parser-format specific options forwarded to concrete parser.
  parser_string_map format_options{};
};

/// Borrowed parser options view allowing overlay without map copying.
struct parse_options_view {
  /// Effective source URI/path associated with content.
  std::string_view uri{};
  /// Base metadata map entries.
  const parser_string_map *extra_meta_base{nullptr};
  /// Optional overlay metadata map entries.
  const parser_string_map *extra_meta_override{nullptr};
  /// Base format options.
  const parser_string_map *format_options_base{nullptr};
  /// Optional overlay format options.
  const parser_string_map *format_options_override{nullptr};

  /// Iterates effective extra-metadata entries with override-last semantics.
  template <typename fn_t> auto for_each_extra_meta(fn_t &&fn) const -> void {
    if (extra_meta_base != nullptr) {
      for (const auto &[key, value] : *extra_meta_base) {
        std::invoke(fn, key, value);
      }
    }
    if (extra_meta_override != nullptr) {
      for (const auto &[key, value] : *extra_meta_override) {
        std::invoke(fn, key, value);
      }
    }
  }

  /// Iterates effective format-options entries with override-last semantics.
  template <typename fn_t> auto for_each_format_option(fn_t &&fn) const -> void {
    if (format_options_base != nullptr) {
      for (const auto &[key, value] : *format_options_base) {
        std::invoke(fn, key, value);
      }
    }
    if (format_options_override != nullptr) {
      for (const auto &[key, value] : *format_options_override) {
        std::invoke(fn, key, value);
      }
    }
  }
};

} // namespace wh::document::parser
