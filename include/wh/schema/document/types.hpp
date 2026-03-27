// Defines schema-level document structures and JSON conversion helpers used
// by document pipelines and external interfaces.
#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/type_traits.hpp"

namespace wh::schema {

/// One sparse-vector entry as `(dimension_index, weight)`.
using sparse_vector_item = std::pair<std::uint32_t, double>;
/// Dense embedding representation.
using dense_vector = std::vector<double>;
/// Sparse embedding representation.
using sparse_vector = std::vector<sparse_vector_item>;

/// Transparent hash alias for document metadata key lookup.
using document_string_hash = wh::core::transparent_string_hash;

/// Transparent equality alias for metadata key lookup.
using document_string_equal = wh::core::transparent_string_equal;

/// Supported metadata value variants for one document metadata key.
using document_metadata_value =
    std::variant<std::nullptr_t, bool, std::int64_t, double, std::string,
                 dense_vector, sparse_vector, std::vector<std::string>>;
/// Metadata map with heterogeneous key lookup support.
using document_metadata_map =
    std::unordered_map<std::string, document_metadata_value,
                       document_string_hash, document_string_equal>;

/// Reserved metadata keys used by built-in retrieval/rerank flows.
namespace document_metadata_keys {
inline constexpr std::string_view score = "_score";
inline constexpr std::string_view sub_index = "_sub_index";
inline constexpr std::string_view dsl = "_dsl";
inline constexpr std::string_view extra_info = "_extra_info";
inline constexpr std::string_view dense_vector = "_dense_vector";
inline constexpr std::string_view sparse_vector = "_sparse_vector";
} // namespace document_metadata_keys

  /// Document entity with content plus optional typed metadata map.
class document {
public:
  document() = default;

  template <typename content_t>
    requires std::constructible_from<std::string, content_t &&>
  explicit document(content_t &&content)
      : content_(std::forward<content_t>(content)) {}

  /// Returns document content.
  [[nodiscard]] auto content() const noexcept -> const std::string & {
    return content_;
  }

  /// Replaces document content.
  template <typename content_t>
    requires std::constructible_from<std::string, content_t &&>
  auto set_content(content_t &&content) -> document & {
    content_ = std::forward<content_t>(content);
    return *this;
  }

  /// Returns metadata map pointer when metadata exists.
  [[nodiscard]] auto metadata() const noexcept
      -> const document_metadata_map * {
    return metadata_ ? std::addressof(*metadata_) : nullptr;
  }

  /// Returns true when metadata key exists.
  [[nodiscard]] auto has_metadata(const std::string_view key) const -> bool {
    if (!metadata_) {
      return false;
    }
    return metadata_->contains(key);
  }

  /// Sets one metadata key with a typed value.
  template <typename key_t, typename value_t>
    requires std::constructible_from<std::string, key_t &&> &&
             std::constructible_from<document_metadata_value, value_t>
  auto set_metadata(key_t &&key, value_t &&value) -> document &
  {
    ensure_metadata().insert_or_assign(std::string{std::forward<key_t>(key)},
                                       document_metadata_value{
                                           std::forward<value_t>(value)});
    return *this;
  }

  /// Reads metadata value, returning `fallback` when missing/type mismatch.
  template <typename value_t>
  [[nodiscard]] auto metadata_or(const std::string_view key,
                                 const value_t &fallback = value_t{}) const
      -> value_t {
    if (!metadata_) {
      return fallback;
    }
    const auto iter = metadata_->find(key);
    if (iter == metadata_->end()) {
      return fallback;
    }
    const auto *typed = std::get_if<value_t>(&iter->second);
    return typed == nullptr ? fallback : *typed;
  }

  /// Returns typed metadata pointer, or `nullptr` when unavailable.
  template <typename value_t>
  [[nodiscard]] auto metadata_ptr(const std::string_view key) const
      -> const value_t * {
    if (!metadata_) {
      return nullptr;
    }
    const auto iter = metadata_->find(key);
    if (iter == metadata_->end()) {
      return nullptr;
    }
    return std::get_if<value_t>(&iter->second);
  }

  /// Returns typed metadata immutable reference wrapped in `result`.
  template <typename value_t>
  [[nodiscard]] auto metadata_cref(const std::string_view key) const
      -> wh::core::result<std::reference_wrapper<const value_t>> {
    if (!metadata_) {
      return wh::core::result<std::reference_wrapper<const value_t>>::failure(
          wh::core::errc::not_found);
    }
    const auto iter = metadata_->find(key);
    if (iter == metadata_->end()) {
      return wh::core::result<std::reference_wrapper<const value_t>>::failure(
          wh::core::errc::not_found);
    }
    const auto *typed = std::get_if<value_t>(&iter->second);
    if (typed == nullptr) {
      return wh::core::result<std::reference_wrapper<const value_t>>::failure(
          wh::core::errc::type_mismatch);
    }
    return std::cref(*typed);
  }

  /// Sets `_score` metadata.
  auto with_score(const double value) -> document & {
    return set_metadata(std::string{document_metadata_keys::score}, value);
  }

  /// Sets `_sub_index` metadata.
  template <typename value_t>
    requires std::constructible_from<std::string, value_t &&>
  auto with_sub_index(value_t &&value) -> document & {
    return set_metadata(std::string{document_metadata_keys::sub_index},
                        std::forward<value_t>(value));
  }

  /// Sets `_dsl` metadata.
  template <typename value_t>
    requires std::constructible_from<std::string, value_t &&>
  auto with_dsl(value_t &&value) -> document & {
    return set_metadata(std::string{document_metadata_keys::dsl},
                        std::forward<value_t>(value));
  }

  /// Sets `_extra_info` metadata.
  template <typename value_t>
    requires std::constructible_from<std::string, value_t &&>
  auto with_extra_info(value_t &&value) -> document & {
    return set_metadata(std::string{document_metadata_keys::extra_info},
                        std::forward<value_t>(value));
  }

  /// Sets `_dense_vector` metadata.
  template <typename value_t>
    requires std::constructible_from<dense_vector, value_t &&>
  auto with_dense_vector(value_t &&value) -> document & {
    return set_metadata(std::string{document_metadata_keys::dense_vector},
                        std::forward<value_t>(value));
  }

  /// Sets `_dense_vector` metadata.
  auto with_dense_vector(std::initializer_list<double> value) -> document & {
    return set_metadata(std::string{document_metadata_keys::dense_vector},
                        dense_vector{value});
  }

  /// Sets `_sparse_vector` metadata.
  template <typename value_t>
    requires std::constructible_from<sparse_vector, value_t &&>
  auto with_sparse_vector(value_t &&value) -> document & {
    return set_metadata(std::string{document_metadata_keys::sparse_vector},
                        std::forward<value_t>(value));
  }

  /// Sets `_sparse_vector` metadata.
  auto with_sparse_vector(
      std::initializer_list<sparse_vector_item> value) -> document & {
    return set_metadata(std::string{document_metadata_keys::sparse_vector},
                        sparse_vector{value});
  }

  /// Reads `_score` metadata.
  [[nodiscard]] auto score() const -> double {
    return metadata_or<double>(document_metadata_keys::score, 0.0);
  }

  /// Reads `_sub_index` metadata.
  [[nodiscard]] auto sub_index() const -> std::string {
    return metadata_or<std::string>(document_metadata_keys::sub_index, "");
  }

  /// Reads `_dsl` metadata.
  [[nodiscard]] auto dsl() const -> std::string {
    return metadata_or<std::string>(document_metadata_keys::dsl, "");
  }

  /// Reads `_extra_info` metadata.
  [[nodiscard]] auto extra_info() const -> std::string {
    return metadata_or<std::string>(document_metadata_keys::extra_info, "");
  }

  /// Reads `_dense_vector` metadata.
  [[nodiscard]] auto get_dense_vector() const -> dense_vector {
    return metadata_or<dense_vector>(document_metadata_keys::dense_vector, {});
  }

  /// Reads `_sparse_vector` metadata.
  [[nodiscard]] auto get_sparse_vector() const -> sparse_vector {
    return metadata_or<sparse_vector>(document_metadata_keys::sparse_vector,
                                      {});
  }

private:
  /// Lazily materializes metadata storage.
  auto ensure_metadata() -> document_metadata_map & {
    if (!metadata_) {
      metadata_.emplace();
    }
    return *metadata_;
  }

  /// Main text payload of the document.
  std::string content_{};
  /// Lazily-created metadata container to avoid map allocation when unused.
  std::optional<document_metadata_map> metadata_{};
};

} // namespace wh::schema
