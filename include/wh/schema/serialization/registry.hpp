// Defines runtime serialization registry APIs for type-name keyed codec
// registration, lookup, and freeze semantics.
#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "wh/core/any.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/type_traits.hpp"
#include "wh/core/json.hpp"
#include "wh/internal/serialization.hpp"
#include "wh/internal/type_name.hpp"

namespace wh::schema {

namespace detail {

/// Transparent hash alias for heterogeneous string lookup.
using transparent_string_hash = wh::core::transparent_string_hash;

/// Transparent equality alias for heterogeneous string lookup.
using transparent_string_equal = wh::core::transparent_string_equal;

} // namespace detail

/// Runtime registry for type-name and serializer/deserializer bindings.
class serialization_registry {
public:
  serialization_registry() = default;

  /// Reserves storage for expected type/name entries.
  auto reserve(const std::size_t type_count, const std::size_t name_count = 0U)
      -> void {
    entries_by_key_.reserve(type_count);
    name_to_entry_.reserve(name_count == 0U ? type_count : name_count);
  }

  /// Freezes registry to reject future registrations.
  auto freeze() noexcept -> void { frozen_ = true; }

  /// Returns true when registry is frozen.
  [[nodiscard]] auto is_frozen() const noexcept -> bool { return frozen_; }

  /// Registers one concrete type with primary name and aliases.
  template <typename type_t>
    requires std::default_initializable<wh::core::remove_cvref_t<type_t>>
  auto register_type(const std::string_view primary_name,
                     const std::initializer_list<std::string_view> aliases = {})
      -> wh::core::result<void> {
    using normalized_t = wh::core::remove_cvref_t<type_t>;
    constexpr auto key = wh::core::any_type_key_v<normalized_t>;

    if (frozen_) {
      return wh::core::result<void>::failure(
          wh::core::errc::contract_violation);
    }
    if (primary_name.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    if (entries_by_key_.contains(key)) {
      return wh::core::result<void>::failure(wh::core::errc::already_exists);
    }
    if (name_to_entry_.find(primary_name) != name_to_entry_.end()) {
      return wh::core::result<void>::failure(wh::core::errc::already_exists);
    }

    std::vector<std::string> normalized_aliases;
    normalized_aliases.reserve(aliases.size());
    for (const auto alias : aliases) {
      if (alias.empty()) {
        return wh::core::result<void>::failure(
            wh::core::errc::invalid_argument);
      }
      const auto duplicate_alias = std::ranges::any_of(
          normalized_aliases, [&](const std::string &normalized_alias) {
            return normalized_alias == alias;
          });
      if (duplicate_alias || name_to_entry_.find(alias) != name_to_entry_.end() ||
          alias == primary_name) {
        return wh::core::result<void>::failure(wh::core::errc::already_exists);
      }
      normalized_aliases.emplace_back(alias);
    }

    serialization_entry entry{};
    entry.primary_name = std::string{primary_name};
    entry.aliases = normalized_aliases;
    entry.key = key;
    entry.encode_any = &encode_from_any<normalized_t>;
    entry.encode_ptr = &encode_from_ptr<normalized_t>;
    entry.decode_any = &decode_to_any<normalized_t>;
    entry.decode_ptr = &decode_to_ptr<normalized_t>;

    auto [iter, inserted] = entries_by_key_.emplace(key, std::move(entry));
    if (!inserted) {
      return wh::core::result<void>::failure(wh::core::errc::already_exists);
    }

    const auto *entry_ptr = &iter->second;
    name_to_entry_.emplace(iter->second.primary_name, entry_ptr);
    std::ranges::for_each(iter->second.aliases, [&](const std::string &alias) {
      name_to_entry_.emplace(alias, entry_ptr);
    });
    return {};
  }

  /// Registers type using diagnostic alias as primary name.
  template <typename type_t>
    requires std::default_initializable<wh::core::remove_cvref_t<type_t>>
  auto register_type_with_diagnostic_alias() -> wh::core::result<void> {
    return register_type<type_t>(
        wh::internal::diagnostic_type_alias<wh::core::remove_cvref_t<type_t>>());
  }

  /// Registers type using persistent alias as primary name.
  template <typename type_t>
    requires std::default_initializable<wh::core::remove_cvref_t<type_t>>
  auto register_type_with_persistent_alias() -> wh::core::result<void> {
    return register_type<type_t>(
        wh::internal::persistent_type_alias<wh::core::remove_cvref_t<type_t>>());
  }

  /// Resolves runtime key by registered name/alias.
  [[nodiscard]] auto key_for_name(const std::string_view name) const
      -> wh::core::result<wh::core::any_type_key> {
    const auto *entry = find_entry_by_name(name);
    if (entry == nullptr) {
      return wh::core::result<wh::core::any_type_key>::failure(
          wh::core::errc::not_found);
    }
    return entry->key;
  }

  /// Resolves primary name by registered runtime key.
  [[nodiscard]] auto primary_name_for_key(const wh::core::any_type_key key) const
      -> wh::core::result<std::string_view> {
    const auto *entry = find_entry(key);
    if (entry == nullptr) {
      return wh::core::result<std::string_view>::failure(
          wh::core::errc::not_found);
    }
    return entry->primary_name;
  }

  /// Serializes `wh::core::any` by explicit type key.
  [[nodiscard]] auto serialize_any(const wh::core::any_type_key key,
                                   const wh::core::any &value) const
      -> wh::core::result<wh::core::json_document> {
    const auto *entry = find_entry(key);
    if (entry == nullptr) {
      return wh::core::result<wh::core::json_document>::failure(
          wh::core::errc::not_found);
    }

    wh::core::json_document output;
    auto encoded = entry->encode_any(value, output, output.GetAllocator());
    if (encoded.has_error()) {
      return wh::core::result<wh::core::json_document>::failure(
          encoded.error());
    }
    return output;
  }

  /// Serializes type-erased pointer by explicit type key.
  [[nodiscard]] auto serialize_view(const wh::core::any_type_key key,
                                    const void *value) const
      -> wh::core::result<wh::core::json_document> {
    if (value == nullptr) {
      return wh::core::result<wh::core::json_document>::failure(
          wh::core::errc::invalid_argument);
    }

    const auto *entry = find_entry(key);
    if (entry == nullptr) {
      return wh::core::result<wh::core::json_document>::failure(
          wh::core::errc::not_found);
    }

    wh::core::json_document output;
    auto encoded = entry->encode_ptr(value, output, output.GetAllocator());
    if (encoded.has_error()) {
      return wh::core::result<wh::core::json_document>::failure(
          encoded.error());
    }
    return output;
  }

  /// Deserializes JSON to type-erased value by registered name.
  [[nodiscard]] auto deserialize_any(const std::string_view name,
                                     const wh::core::json_value &input) const
      -> wh::core::result<wh::core::any> {
    const auto *entry = find_entry_by_name(name);
    if (entry == nullptr) {
      return wh::core::result<wh::core::any>::failure(
          wh::core::errc::not_found);
    }
    return entry->decode_any(input);
  }

  /// Deserializes JSON into caller-provided output buffer.
  auto deserialize_to(const std::string_view name,
                      const wh::core::any_type_key expected_key,
                      const wh::core::json_value &input, void *output) const
      -> wh::core::result<void> {
    if (output == nullptr) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }

    const auto *entry = find_entry_by_name(name);
    if (entry == nullptr) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    if (entry->key != expected_key) {
      return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
    }
    return entry->decode_ptr(input, output);
  }

  /// Serializes typed value through the runtime registration table.
  template <typename type_t>
  [[nodiscard]] auto serialize(const type_t &value) const
      -> wh::core::result<wh::core::json_document> {
    using normalized_t = wh::core::remove_cvref_t<type_t>;
    return serialize_view(wh::core::any_type_key_v<normalized_t>, &value);
  }

  /// Deserializes JSON into `type_t` and validates registered name-to-type match.
  template <typename type_t>
  [[nodiscard]] auto deserialize(const std::string_view name,
                                 const wh::core::json_value &input) const
      -> wh::core::result<type_t> {
    using normalized_t = wh::core::remove_cvref_t<type_t>;
    normalized_t output{};
    auto decoded =
        deserialize_to(name, wh::core::any_type_key_v<normalized_t>, input,
                       &output);
    if (decoded.has_error()) {
      return wh::core::result<type_t>::failure(decoded.error());
    }
    return output;
  }

  /// Number of registered concrete types.
  [[nodiscard]] auto size() const noexcept -> std::size_t {
    return entries_by_key_.size();
  }

private:
  /// Signature for encoding from `wh::core::any`.
  using encode_any_function = wh::core::result<void> (*)(
      const wh::core::any &, wh::core::json_value &,
      wh::core::json_allocator &);
  /// Signature for encoding from type-erased pointer.
  using encode_ptr_function = wh::core::result<void> (*)(
      const void *, wh::core::json_value &, wh::core::json_allocator &);
  /// Signature for decoding to `wh::core::any`.
  using decode_any_function =
      wh::core::result<wh::core::any> (*)(const wh::core::json_value &);
  /// Signature for decoding into type-erased pointer.
  using decode_ptr_function =
      wh::core::result<void> (*)(const wh::core::json_value &, void *);

  /// Internal function table for one registered type.
  struct serialization_entry {
    /// Canonical registered type name.
    std::string primary_name{};
    /// Alternative lookup names.
    std::vector<std::string> aliases{};
    /// Runtime payload key for dispatch and safety checks.
    wh::core::any_type_key key{};
    /// Encoder bound to `wh::core::any` input.
    encode_any_function encode_any{nullptr};
    /// Encoder bound to type-erased pointer input.
    encode_ptr_function encode_ptr{nullptr};
    /// Decoder producing type-erased value.
    decode_any_function decode_any{nullptr};
    /// Decoder writing into caller-provided pointer.
    decode_ptr_function decode_ptr{nullptr};
  };

  /// Encodes `type_t` value extracted from `wh::core::any`.
  template <typename type_t>
  static auto encode_from_any(const wh::core::any &value,
                              wh::core::json_value &output,
                              wh::core::json_allocator &allocator)
      -> wh::core::result<void> {
    const auto *typed = wh::core::any_cast<type_t>(&value);
    if (typed == nullptr) {
      return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
    }
    return wh::internal::to_json(*typed, output, allocator);
  }

  /// Encodes `type_t` from type-erased pointer.
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

  /// Decodes JSON into `wh::core::any` containing `type_t`.
  template <typename type_t>
  static auto decode_to_any(const wh::core::json_value &input)
      -> wh::core::result<wh::core::any> {
    auto decoded = wh::internal::from_json_value<type_t>(input);
    if (decoded.has_error()) {
      return wh::core::result<wh::core::any>::failure(decoded.error());
    }
    return wh::core::any{std::move(decoded).value()};
  }

  /// Decodes JSON into caller-provided `type_t` storage.
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

  /// Finds entry by runtime key.
  [[nodiscard]] auto find_entry(const wh::core::any_type_key key) const noexcept
      -> const serialization_entry * {
    const auto iter = entries_by_key_.find(key);
    return iter == entries_by_key_.end() ? nullptr : &iter->second;
  }

  /// Finds entry by primary name or alias.
  [[nodiscard]] auto find_entry_by_name(const std::string_view name) const
      noexcept -> const serialization_entry * {
    const auto iter = name_to_entry_.find(name);
    return iter == name_to_entry_.end() ? nullptr : iter->second;
  }

  /// Main registry indexed by concrete runtime key.
  std::unordered_map<wh::core::any_type_key, serialization_entry,
                     wh::core::any_type_key_hash>
      entries_by_key_{};
  /// Name/alias lookup table pointing to entries in `entries_by_key_`.
  std::unordered_map<std::string, const serialization_entry *,
                     detail::transparent_string_hash,
                     detail::transparent_string_equal>
      name_to_entry_{};
  /// Set after freeze to reject further registrations.
  bool frozen_{false};
};

} // namespace wh::schema
