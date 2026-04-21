// Defines extension-based parser registry and dispatch helpers that route
// documents to concrete parser implementations by URI extension.
#pragma once

#include <algorithm>
#include <concepts>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "wh/core/error.hpp"
#include "wh/core/type_traits.hpp"
#include "wh/document/parser/interface.hpp"
#include "wh/document/parser/text_parser.hpp"

namespace wh::document::parser {

class ext_parser {
public:
  using parser_registry_map =
      std::unordered_map<std::string, parser, wh::core::transparent_string_hash,
                         wh::core::transparent_string_equal>;

  ext_parser() = default;
  explicit ext_parser(const parser &fallback) : fallback_(fallback) {}
  explicit ext_parser(parser &&fallback) : fallback_(std::move(fallback)) {}

  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return wh::core::component_descriptor{"ExtParser", wh::core::component_kind::document};
  }

  auto register_parser(const std::string &extension, const parser &value)
      -> wh::core::result<void> {
    if (extension.empty() || !value.has_value()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    return register_parser_impl(std::string{extension}, value);
  }

  auto register_parser(std::string &&extension, parser &&value) -> wh::core::result<void> {
    if (extension.empty() || !value.has_value()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    return register_parser_impl(std::move(extension), std::move(value));
  }

  template <typename parser_t, typename extension_t, typename... args_t>
    requires parser_like<wh::core::remove_cvref_t<parser_t>> &&
             std::constructible_from<wh::core::remove_cvref_t<parser_t>, args_t...> &&
             std::constructible_from<std::string, extension_t &&>
  auto register_parser(extension_t &&extension, args_t &&...args) -> wh::core::result<void> {
    return register_parser(
        std::forward<extension_t>(extension),
        parser{wh::core::remove_cvref_t<parser_t>{std::forward<args_t>(args)...}});
  }

  auto set_fallback(const parser &fallback) -> void { fallback_ = fallback; }
  auto set_fallback(parser &&fallback) -> void { fallback_ = std::move(fallback); }

  template <typename parser_t, typename... args_t>
    requires parser_like<wh::core::remove_cvref_t<parser_t>> &&
             std::constructible_from<wh::core::remove_cvref_t<parser_t>, args_t...>
  auto set_fallback(args_t &&...args) -> void {
    fallback_ = parser{wh::core::remove_cvref_t<parser_t>{std::forward<args_t>(args)...}};
  }

  auto clear_fallback() noexcept -> void { fallback_.reset(); }

  [[nodiscard]] auto parser_registry_copy() const -> parser_registry_map { return parsers_; }

  [[nodiscard]] auto parse(const parse_request &request) const -> wh::core::result<document_batch> {
    return parse_by_extension(request, request.options.uri);
  }

  [[nodiscard]] auto parse(parse_request &&request) const -> wh::core::result<document_batch> {
    const std::string source_uri = request.options.uri;
    return parse_by_extension(std::move(request), source_uri);
  }

  [[nodiscard]] auto parse(const parse_request_view request) const
      -> wh::core::result<document_batch> {
    return parse_by_extension(request, request.options.uri);
  }

private:
  template <typename extension_t>
    requires std::constructible_from<std::string, extension_t &&>
  auto register_parser_impl(extension_t &&extension, const parser &value)
      -> wh::core::result<void> {
    auto normalized = normalize_extension(std::forward<extension_t>(extension));
    parsers_.insert_or_assign(std::move(normalized), value);
    return {};
  }

  template <typename extension_t>
    requires std::constructible_from<std::string, extension_t &&>
  auto register_parser_impl(extension_t &&extension, parser &&value) -> wh::core::result<void> {
    auto normalized = normalize_extension(std::forward<extension_t>(extension));
    parsers_.insert_or_assign(std::move(normalized), std::move(value));
    return {};
  }

  template <typename request_t>
  [[nodiscard]] auto parse_with(const parser &selected, request_t &&request) const
      -> wh::core::result<document_batch> {
    if (!selected.has_value()) {
      return wh::core::result<document_batch>::failure(wh::core::errc::not_supported);
    }
    return selected.parse(std::forward<request_t>(request));
  }

  template <typename request_t>
  [[nodiscard]] auto parse_and_strip(const parser &selected, request_t &&request) const
      -> wh::core::result<document_batch> {
    auto parsed = parse_with(selected, std::forward<request_t>(request));
    if (parsed.has_error()) {
      return parsed;
    }
    return strip_empty_documents(std::move(parsed).value());
  }

  template <typename request_t>
  [[nodiscard]] auto parse_by_extension(request_t &&request, const std::string_view uri) const
      -> wh::core::result<document_batch> {
    const auto extension = extract_extension_view(uri);
    const auto iter = parsers_.find(extension);
    if (iter != parsers_.end()) {
      return parse_and_strip(iter->second, std::forward<request_t>(request));
    }

    if (fallback_.has_value()) {
      return parse_and_strip(*fallback_, std::forward<request_t>(request));
    }

    return wh::core::result<document_batch>::failure(wh::core::errc::not_found);
  }

  template <typename extension_t>
    requires std::constructible_from<std::string, extension_t &&>
  [[nodiscard]] static auto normalize_extension(extension_t &&extension) -> std::string {
    std::string normalized{std::forward<extension_t>(extension)};
    if (!normalized.empty() && normalized.front() == '.') {
      normalized.erase(normalized.begin());
    }
    return normalized;
  }

  [[nodiscard]] static auto extract_extension_view(const std::string_view uri) -> std::string_view {
    const auto slash = uri.find_last_of("/\\");
    const auto file_name = (slash == std::string_view::npos) ? uri : uri.substr(slash + 1U);
    const auto dot = file_name.find_last_of('.');
    if (dot == std::string_view::npos || dot + 1U >= file_name.size()) {
      return {};
    }
    return file_name.substr(dot + 1U);
  }

  [[nodiscard]] static auto strip_empty_documents(document_batch docs)
      -> wh::core::result<document_batch> {
    const auto remove_begin = std::ranges::remove_if(
        docs, [](const wh::schema::document &doc) { return doc.content().empty(); });
    docs.erase(remove_begin.begin(), remove_begin.end());
    return docs;
  }

  parser_registry_map parsers_{};
  std::optional<parser> fallback_{make_text_parser()};
};

} // namespace wh::document::parser
