#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"

namespace wh::schema {

using sparse_vector_item = std::pair<std::uint32_t, double>;
using dense_vector = std::vector<double>;
using sparse_vector = std::vector<sparse_vector_item>;

struct document_string_hash {
  using is_transparent = void;

  [[nodiscard]] auto operator()(const std::string_view value) const noexcept
      -> std::size_t {
    return std::hash<std::string_view>{}(value);
  }

  [[nodiscard]] auto operator()(const std::string &value) const noexcept
      -> std::size_t {
    return (*this)(std::string_view{value});
  }

  [[nodiscard]] auto operator()(const char *value) const noexcept
      -> std::size_t {
    return (*this)(std::string_view{value});
  }
};

struct document_string_equal {
  using is_transparent = void;

  [[nodiscard]] auto operator()(const std::string_view left,
                                const std::string_view right) const noexcept
      -> bool {
    return left == right;
  }
};

using document_metadata_value =
    std::variant<std::nullptr_t, bool, std::int64_t, double, std::string,
                 dense_vector, sparse_vector, std::vector<std::string>>;
using document_metadata_map =
    std::unordered_map<std::string, document_metadata_value,
                       document_string_hash, document_string_equal>;

namespace document_metadata_keys {
inline constexpr std::string_view score = "_score";
inline constexpr std::string_view sub_index = "_sub_index";
inline constexpr std::string_view dsl = "_dsl";
inline constexpr std::string_view extra_info = "_extra_info";
inline constexpr std::string_view dense_vector = "_dense_vector";
inline constexpr std::string_view sparse_vector = "_sparse_vector";
} // namespace document_metadata_keys

class document {
public:
  document() = default;
  explicit document(std::string content) : content_(std::move(content)) {}

  [[nodiscard]] auto content() const noexcept -> const std::string & {
    return content_;
  }

  auto set_content(std::string content) -> document & {
    content_ = std::move(content);
    return *this;
  }

  [[nodiscard]] auto metadata() const noexcept
      -> const document_metadata_map * {
    return metadata_ ? std::addressof(*metadata_) : nullptr;
  }

  [[nodiscard]] auto has_metadata(const std::string_view key) const -> bool {
    if (!metadata_) {
      return false;
    }
    return metadata_->contains(key);
  }

  template <typename value_t>
  auto set_metadata(std::string key, value_t &&value) -> document &
    requires std::constructible_from<document_metadata_value, value_t>
  {
    ensure_metadata().insert_or_assign(
        std::move(key), document_metadata_value{std::forward<value_t>(value)});
    return *this;
  }

  template <typename value_t>
  [[nodiscard]] auto metadata_or(const std::string_view key,
                                 value_t fallback = value_t{}) const
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

  auto with_score(const double value) -> document & {
    return set_metadata(std::string{document_metadata_keys::score}, value);
  }

  auto with_sub_index(std::string value) -> document & {
    return set_metadata(std::string{document_metadata_keys::sub_index},
                        std::move(value));
  }

  auto with_dsl(std::string value) -> document & {
    return set_metadata(std::string{document_metadata_keys::dsl},
                        std::move(value));
  }

  auto with_extra_info(std::string value) -> document & {
    return set_metadata(std::string{document_metadata_keys::extra_info},
                        std::move(value));
  }

  auto with_dense_vector(dense_vector value) -> document & {
    return set_metadata(std::string{document_metadata_keys::dense_vector},
                        std::move(value));
  }

  auto with_sparse_vector(sparse_vector value) -> document & {
    return set_metadata(std::string{document_metadata_keys::sparse_vector},
                        std::move(value));
  }

  [[nodiscard]] auto score() const -> double {
    return metadata_or<double>(document_metadata_keys::score, 0.0);
  }

  [[nodiscard]] auto sub_index() const -> std::string {
    return metadata_or<std::string>(document_metadata_keys::sub_index, "");
  }

  [[nodiscard]] auto dsl() const -> std::string {
    return metadata_or<std::string>(document_metadata_keys::dsl, "");
  }

  [[nodiscard]] auto extra_info() const -> std::string {
    return metadata_or<std::string>(document_metadata_keys::extra_info, "");
  }

  [[nodiscard]] auto get_dense_vector() const -> dense_vector {
    return metadata_or<dense_vector>(document_metadata_keys::dense_vector, {});
  }

  [[nodiscard]] auto get_sparse_vector() const -> sparse_vector {
    return metadata_or<sparse_vector>(document_metadata_keys::sparse_vector,
                                      {});
  }

private:
  auto ensure_metadata() -> document_metadata_map & {
    if (!metadata_) {
      metadata_.emplace();
    }
    return *metadata_;
  }

  std::string content_{};
  std::optional<document_metadata_map> metadata_{};
};

} // namespace wh::schema
