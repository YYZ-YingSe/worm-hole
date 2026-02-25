#pragma once

#include <any>
#include <cstddef>
#include <concepts>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/types/json_types.hpp"
#include "wh/internal/serialization.hpp"
#include "wh/internal/type_name.hpp"

namespace wh::schema {

namespace detail {

struct transparent_string_hash {
  using is_transparent = void;

  [[nodiscard]] auto operator()(const std::string_view value) const noexcept
      -> std::size_t {
    return std::hash<std::string_view>{}(value);
  }

  [[nodiscard]] auto operator()(const std::string &value) const noexcept
      -> std::size_t {
    return (*this)(std::string_view{value});
  }
};

struct transparent_string_equal {
  using is_transparent = void;

  [[nodiscard]] auto operator()(const std::string_view lhs,
                                const std::string_view rhs) const noexcept
      -> bool {
    return lhs == rhs;
  }
};

} // namespace detail

class serialization_registry {
public:
  serialization_registry() = default;

  auto reserve(const std::size_t type_count,
               const std::size_t name_count = 0U) -> void {
    entries_by_type_.reserve(type_count);
    name_to_type_.reserve(name_count == 0U ? type_count : name_count);
  }

  auto freeze() noexcept -> void {
    frozen_ = true;
  }

  [[nodiscard]] auto is_frozen() const noexcept -> bool {
    return frozen_;
  }

  template <typename type_t>
    requires std::default_initializable<std::remove_cvref_t<type_t>>
  auto register_type(
      const std::string_view primary_name,
      const std::initializer_list<std::string_view> aliases = {})
      -> wh::core::result<void> {
    using normalized_t = std::remove_cvref_t<type_t>;
    const auto type = std::type_index(typeid(normalized_t));

    if (frozen_) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    if (primary_name.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    if (entries_by_type_.contains(type)) {
      return wh::core::result<void>::failure(wh::core::errc::already_exists);
    }
    if (name_to_type_.find(primary_name) != name_to_type_.end()) {
      return wh::core::result<void>::failure(wh::core::errc::already_exists);
    }

    std::vector<std::string> normalized_aliases;
    normalized_aliases.reserve(aliases.size());
    for (const auto alias : aliases) {
      if (alias.empty()) {
        return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
      }
      if (name_to_type_.find(alias) != name_to_type_.end() ||
          alias == primary_name) {
        return wh::core::result<void>::failure(wh::core::errc::already_exists);
      }
      normalized_aliases.emplace_back(alias);
    }

    serialization_entry entry{};
    entry.primary_name = std::string{primary_name};
    entry.aliases = normalized_aliases;
    entry.type = type;
    entry.encode_any = &encode_from_any<normalized_t>;
    entry.encode_ptr = &encode_from_ptr<normalized_t>;
    entry.decode_any = &decode_to_any<normalized_t>;
    entry.decode_ptr = &decode_to_ptr<normalized_t>;

    auto [iter, inserted] = entries_by_type_.emplace(type, std::move(entry));
    if (!inserted) {
      return wh::core::result<void>::failure(wh::core::errc::already_exists);
    }

    name_to_type_.emplace(iter->second.primary_name, type);
    for (const auto &alias : iter->second.aliases) {
      name_to_type_.emplace(alias, type);
    }
    return {};
  }

  template <typename type_t>
    requires std::default_initializable<std::remove_cvref_t<type_t>>
  auto register_type_with_diagnostic_alias() -> wh::core::result<void> {
    return register_type<type_t>(
        wh::internal::diagnostic_type_alias<std::remove_cvref_t<type_t>>());
  }

  template <typename type_t>
    requires std::default_initializable<std::remove_cvref_t<type_t>>
  auto register_type_with_persistent_alias() -> wh::core::result<void> {
    return register_type<type_t>(
        wh::internal::persistent_type_alias<std::remove_cvref_t<type_t>>());
  }

  [[nodiscard]] auto type_for_name(const std::string_view name) const
      -> wh::core::result<std::type_index> {
    const auto *type = find_type(name);
    if (type == nullptr) {
      return wh::core::result<std::type_index>::failure(wh::core::errc::not_found);
    }
    return *type;
  }

  [[nodiscard]] auto primary_name_for_type(const std::type_index type) const
      -> wh::core::result<std::string_view> {
    const auto *entry = find_entry(type);
    if (entry == nullptr) {
      return wh::core::result<std::string_view>::failure(wh::core::errc::not_found);
    }
    return entry->primary_name;
  }

  [[nodiscard]] auto serialize_any(const std::type_index type,
                                   const std::any &value) const
      -> wh::core::result<wh::core::json_document> {
    const auto *entry = find_entry(type);
    if (entry == nullptr) {
      return wh::core::result<wh::core::json_document>::failure(
          wh::core::errc::not_found);
    }

    wh::core::json_document output;
    auto encoded = entry->encode_any(value, output, output.GetAllocator());
    if (encoded.has_error()) {
      return wh::core::result<wh::core::json_document>::failure(encoded.error());
    }
    return output;
  }

  [[nodiscard]] auto serialize_view(const std::type_index type,
                                    const void *value) const
      -> wh::core::result<wh::core::json_document> {
    if (value == nullptr) {
      return wh::core::result<wh::core::json_document>::failure(
          wh::core::errc::invalid_argument);
    }

    const auto *entry = find_entry(type);
    if (entry == nullptr) {
      return wh::core::result<wh::core::json_document>::failure(
          wh::core::errc::not_found);
    }

    wh::core::json_document output;
    auto encoded = entry->encode_ptr(value, output, output.GetAllocator());
    if (encoded.has_error()) {
      return wh::core::result<wh::core::json_document>::failure(encoded.error());
    }
    return output;
  }

  [[nodiscard]] auto deserialize_any(const std::string_view name,
                                     const wh::core::json_value &input) const
      -> wh::core::result<std::any> {
    const auto *type = find_type(name);
    if (type == nullptr) {
      return wh::core::result<std::any>::failure(wh::core::errc::not_found);
    }

    const auto *entry = find_entry(*type);
    if (entry == nullptr) {
      return wh::core::result<std::any>::failure(wh::core::errc::not_found);
    }
    return entry->decode_any(input);
  }

  auto deserialize_to(const std::string_view name,
                      const std::type_index expected_type,
                      const wh::core::json_value &input, void *output) const
      -> wh::core::result<void> {
    if (output == nullptr) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }

    const auto *type = find_type(name);
    if (type == nullptr) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    if (*type != expected_type) {
      return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
    }

    const auto *entry = find_entry(*type);
    if (entry == nullptr) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    return entry->decode_ptr(input, output);
  }

  template <typename type_t>
  [[nodiscard]] auto serialize(const type_t &value) const
      -> wh::core::result<wh::core::json_document> {
    using normalized_t = std::remove_cvref_t<type_t>;
    return serialize_view(std::type_index(typeid(normalized_t)), &value);
  }

  template <typename type_t>
  [[nodiscard]] auto deserialize(const std::string_view name,
                                 const wh::core::json_value &input) const
      -> wh::core::result<type_t> {
    using normalized_t = std::remove_cvref_t<type_t>;
    normalized_t output{};
    auto decoded = deserialize_to(name, std::type_index(typeid(normalized_t)),
                                  input, &output);
    if (decoded.has_error()) {
      return wh::core::result<type_t>::failure(decoded.error());
    }
    return output;
  }

  [[nodiscard]] auto size() const noexcept -> std::size_t {
    return entries_by_type_.size();
  }

private:
  using encode_any_function = wh::core::result<void> (*)(
      const std::any &, wh::core::json_value &, wh::core::json_allocator &);
  using encode_ptr_function = wh::core::result<void> (*)(
      const void *, wh::core::json_value &, wh::core::json_allocator &);
  using decode_any_function =
      wh::core::result<std::any> (*)(const wh::core::json_value &);
  using decode_ptr_function =
      wh::core::result<void> (*)(const wh::core::json_value &, void *);

  struct serialization_entry {
    std::string primary_name{};
    std::vector<std::string> aliases{};
    std::type_index type{typeid(void)};
    encode_any_function encode_any{nullptr};
    encode_ptr_function encode_ptr{nullptr};
    decode_any_function decode_any{nullptr};
    decode_ptr_function decode_ptr{nullptr};
  };

  template <typename type_t>
  static auto encode_from_any(const std::any &value, wh::core::json_value &output,
                              wh::core::json_allocator &allocator)
      -> wh::core::result<void> {
    const auto *typed = std::any_cast<type_t>(&value);
    if (typed == nullptr) {
      return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
    }
    return wh::internal::to_json(*typed, output, allocator);
  }

  template <typename type_t>
  static auto encode_from_ptr(const void *value, wh::core::json_value &output,
                              wh::core::json_allocator &allocator)
      -> wh::core::result<void> {
    if (value == nullptr) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    return wh::internal::to_json(*static_cast<const type_t *>(value), output,
                                 allocator);
  }

  template <typename type_t>
  static auto decode_to_any(const wh::core::json_value &input)
      -> wh::core::result<std::any> {
    auto decoded = wh::internal::from_json_value<type_t>(input);
    if (decoded.has_error()) {
      return wh::core::result<std::any>::failure(decoded.error());
    }
    return std::any{std::move(decoded).value()};
  }

  template <typename type_t>
  static auto decode_to_ptr(const wh::core::json_value &input, void *output)
      -> wh::core::result<void> {
    if (output == nullptr) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    auto decoded = wh::internal::from_json_value<type_t>(input);
    if (decoded.has_error()) {
      return wh::core::result<void>::failure(decoded.error());
    }
    *static_cast<type_t *>(output) = std::move(decoded).value();
    return {};
  }

  [[nodiscard]] auto find_entry(const std::type_index type) const noexcept
      -> const serialization_entry * {
    const auto iter = entries_by_type_.find(type);
    return iter == entries_by_type_.end() ? nullptr : &iter->second;
  }

  [[nodiscard]] auto find_type(const std::string_view name) const noexcept
      -> const std::type_index * {
    const auto iter = name_to_type_.find(name);
    return iter == name_to_type_.end() ? nullptr : &iter->second;
  }

  std::unordered_map<std::type_index, serialization_entry> entries_by_type_{};
  std::unordered_map<std::string, std::type_index, detail::transparent_string_hash,
                     detail::transparent_string_equal>
      name_to_type_{};
  bool frozen_{false};
};

} // namespace wh::schema
