// Defines the default owning any handle used by core-level runtime payloads.
#pragma once

#include <cassert>
#include <concepts>
#include <type_traits>
#include <utility>

#include "wh/core/any/basic_any.hpp"

namespace wh::core {

using any = basic_any<>;

template <typename value_t>
inline constexpr auto any_type_key_v = any::template type_key<value_t>();

template <typename value_t>
inline constexpr const any_type_info &any_info_v =
    any::template info_of<value_t>();

template <typename value_t, typename... arg_ts>
[[nodiscard]] inline auto make_any(arg_ts &&...args) -> any {
  return any{std::in_place_type<value_t>, std::forward<arg_ts>(args)...};
}

template <typename value_t>
[[nodiscard]] inline auto forward_as_any(value_t &&value) noexcept -> any {
  return any{std::in_place_type<value_t &&>, std::forward<value_t>(value)};
}

template <typename map_t>
concept any_mapped_map =
    requires {
      typename std::remove_cvref_t<map_t>::mapped_type;
    } && std::same_as<typename std::remove_cvref_t<map_t>::mapped_type, any>;

template <any_mapped_map map_t>
[[nodiscard]] inline auto into_owned_any_map(const map_t &values)
    -> wh::core::result<std::remove_cvref_t<map_t>> {
  using owned_map_t = std::remove_cvref_t<map_t>;
  owned_map_t owned{};
  owned.reserve(values.size());
  for (const auto &[key, value] : values) {
    auto prepared = wh::core::into_owned(value);
    if (prepared.has_error()) {
      return wh::core::result<owned_map_t>::failure(prepared.error());
    }
    owned.insert_or_assign(key, std::move(prepared).value());
  }
  return owned;
}

template <any_mapped_map map_t>
[[nodiscard]] inline auto into_owned_any_map(map_t &&values)
    -> wh::core::result<std::remove_cvref_t<map_t>> {
  using owned_map_t = std::remove_cvref_t<map_t>;
  owned_map_t owned{};
  owned.reserve(values.size());
  for (auto iter = values.begin(); iter != values.end();) {
    auto node = values.extract(iter++);
    auto prepared = wh::core::into_owned(std::move(node.mapped()));
    if (prepared.has_error()) {
      return wh::core::result<owned_map_t>::failure(prepared.error());
    }
    node.mapped() = std::move(prepared).value();
    owned.insert(std::move(node));
  }
  return owned;
}

template <typename value_t>
[[nodiscard]] inline auto any_cast(any *value) noexcept -> value_t * {
  if (value == nullptr) {
    return nullptr;
  }
  if constexpr (std::is_const_v<value_t>) {
    return any_cast<value_t>(&std::as_const(*value));
  } else {
    return value->template data<value_t>();
  }
}

template <typename value_t>
[[nodiscard]] inline auto any_cast(const any *value) noexcept
    -> const value_t * {
  if (value == nullptr) {
    return nullptr;
  }
  return value->template get_if<value_t>();
}

template <typename value_t>
[[nodiscard]] inline auto any_cast(any &value) noexcept -> value_t {
  using stored_t = std::remove_reference_t<const value_t>;
  auto *typed = any_cast<stored_t>(&value);
  assert(typed != nullptr);
  if constexpr (std::is_lvalue_reference_v<value_t>) {
    return static_cast<value_t>(*typed);
  } else {
    return static_cast<std::remove_cvref_t<value_t>>(*typed);
  }
}

template <typename value_t>
[[nodiscard]] inline auto any_cast(const any &value) noexcept -> value_t {
  using stored_t = std::remove_reference_t<value_t>;
  auto *typed = any_cast<stored_t>(&value);
  assert(typed != nullptr);
  if constexpr (std::is_lvalue_reference_v<value_t>) {
    return static_cast<value_t>(*typed);
  } else {
    return static_cast<std::remove_cvref_t<value_t>>(*typed);
  }
}

template <typename value_t>
[[nodiscard]] inline auto any_cast(any &&value) noexcept -> value_t {
  using stored_t = std::remove_reference_t<value_t>;
  if constexpr (std::is_copy_constructible_v<std::remove_cvref_t<value_t>>) {
    if (auto *typed = any_cast<stored_t>(&value); typed != nullptr) {
      if constexpr (std::is_lvalue_reference_v<value_t>) {
        return static_cast<value_t>(*typed);
      } else {
        return static_cast<std::remove_cvref_t<value_t>>(std::move(*typed));
      }
    }
    return any_cast<value_t>(value);
  } else {
    auto *typed = any_cast<stored_t>(&value);
    assert(typed != nullptr);
    if constexpr (std::is_lvalue_reference_v<value_t>) {
      return static_cast<value_t>(*typed);
    } else {
      return static_cast<std::remove_cvref_t<value_t>>(std::move(*typed));
    }
  }
}

template <typename value_t>
[[nodiscard]] inline auto any_cast(const any &&value) noexcept -> value_t {
  using stored_t = std::remove_reference_t<value_t>;
  auto *typed = any_cast<stored_t>(&value);
  assert(typed != nullptr);
  if constexpr (std::is_lvalue_reference_v<value_t>) {
    return static_cast<value_t>(*typed);
  } else {
    return static_cast<std::remove_cvref_t<value_t>>(*typed);
  }
}

} // namespace wh::core
