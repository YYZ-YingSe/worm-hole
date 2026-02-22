#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "wh/core/type_utils.hpp"
#include "wh/internal/type_name.hpp"

namespace wh::core {

namespace detail {

template <typename owner_t, typename... bindings_t>
consteval bool field_names_non_empty(const bindings_t&... bindings) {
  return ((!bindings.name.empty()) && ...);
}

template <typename owner_t, typename... bindings_t>
consteval bool field_names_unique(const bindings_t&... bindings) {
  constexpr std::size_t count = sizeof...(bindings_t);
  std::array<std::string_view, count> names{bindings.name...};
  for (std::size_t i = 0; i < count; ++i) {
    for (std::size_t j = i + 1U; j < count; ++j) {
      if (names[i] == names[j]) {
        return false;
      }
    }
  }
  return true;
}

template <typename owner_t, typename... bindings_t>
consteval bool field_keys_unique(const bindings_t&... bindings) {
  constexpr std::size_t count = sizeof...(bindings_t);
  std::array<std::uint64_t, count> keys{bindings.key...};
  for (std::size_t i = 0; i < count; ++i) {
    for (std::size_t j = i + 1U; j < count; ++j) {
      if (keys[i] == keys[j]) {
        return false;
      }
    }
  }
  return true;
}

template <typename owner_t, typename... bindings_t, std::size_t... index_t>
[[nodiscard]] constexpr auto field_name_array_impl(
    const std::tuple<bindings_t...>& bindings,
    std::index_sequence<index_t...>) noexcept {
  return std::array<std::string_view, sizeof...(bindings_t)>{
      std::get<index_t>(bindings).name...,
  };
}

template <typename owner_t, typename... bindings_t, std::size_t... index_t>
[[nodiscard]] constexpr auto field_key_array_impl(
    const std::tuple<bindings_t...>& bindings,
    std::index_sequence<index_t...>) noexcept {
  return std::array<std::uint64_t, sizeof...(bindings_t)>{
      std::get<index_t>(bindings).key...,
  };
}

}  // namespace detail

struct type_key {
  std::uint64_t value{};

  friend constexpr bool operator==(const type_key&, const type_key&) = default;
};

template <typename t>
[[nodiscard]] constexpr type_key make_type_key() noexcept {
  return type_key{::wh::internal::persistent_type_hash<t>()};
}

template <typename owner_t, typename value_t>
struct field_binding {
  using owner_type = owner_t;
  using value_type = value_t;

  std::string_view name{};
  std::uint64_t key{};
  value_t owner_t::* member{};
};

template <typename owner_t, typename value_t>
[[nodiscard]] constexpr auto field(std::string_view name,
                                   value_t owner_t::* member) noexcept
    -> field_binding<owner_t, value_t> {
  return field_binding<owner_t, value_t>{
      name,
      ::wh::internal::stable_name_hash(name),
      member,
  };
}

template <typename owner_t, typename... bindings_t>
  requires(sizeof...(bindings_t) > 0U)
struct field_map {
  using owner_type = owner_t;

  std::tuple<bindings_t...> bindings;

  [[nodiscard]] static consteval std::size_t size() noexcept {
    return sizeof...(bindings_t);
  }

  [[nodiscard]] constexpr auto names() const noexcept {
    return detail::field_name_array_impl<owner_t, bindings_t...>(
        bindings,
        std::index_sequence_for<bindings_t...>{});
  }

  [[nodiscard]] constexpr auto keys() const noexcept {
    return detail::field_key_array_impl<owner_t, bindings_t...>(
        bindings,
        std::index_sequence_for<bindings_t...>{});
  }
};

template <typename owner_t, typename... bindings_t>
[[nodiscard]] constexpr bool validate_field_map(bindings_t... bindings) noexcept {
  static_assert((std::is_same_v<owner_t, typename bindings_t::owner_type> && ...),
                "field owner type mismatch");
  return detail::field_names_non_empty<owner_t>(bindings...) &&
         detail::field_names_unique<owner_t>(bindings...) &&
         detail::field_keys_unique<owner_t>(bindings...);
}

template <typename owner_t, typename... bindings_t>
[[nodiscard]] constexpr auto make_field_map(bindings_t... bindings) noexcept
    -> field_map<owner_t, bindings_t...> {
  static_assert((std::is_same_v<owner_t, typename bindings_t::owner_type> && ...),
                "field owner type mismatch");
  return field_map<owner_t, bindings_t...>{std::tuple<bindings_t...>{bindings...}};
}

template <typename owner_t, typename value_t>
[[nodiscard]] constexpr auto field_ref(owner_t& object,
                                       const field_binding<owner_t, value_t>& binding) noexcept
    -> value_t& {
  return object.*(binding.member);
}

template <typename owner_t, typename value_t>
[[nodiscard]] constexpr auto field_ref(const owner_t& object,
                                       const field_binding<owner_t, value_t>& binding) noexcept
    -> const value_t& {
  return object.*(binding.member);
}

template <typename owner_t, typename... bindings_t, typename fn_t>
constexpr void for_each_field(const field_map<owner_t, bindings_t...>& map,
                              fn_t&& fn) {
  std::apply(
      [&](const auto&... binding) {
        (std::invoke(fn, binding), ...);
      },
      map.bindings);
}

template <typename owner_t, typename... bindings_t, typename fn_t>
[[nodiscard]] constexpr bool visit_field(const field_map<owner_t, bindings_t...>& map,
                                         const std::string_view name,
                                         fn_t&& fn) {
  bool found = false;
  std::apply(
      [&](const auto&... binding) {
        (([&] {
           using binding_t = std::remove_cvref_t<decltype(binding)>;
           if constexpr (std::invocable<fn_t&, const binding_t&>) {
             if (!found && binding.name == name) {
               std::invoke(fn, binding);
               found = true;
             }
           }
         }()),
         ...);
      },
      map.bindings);
  return found;
}

template <typename owner_t, typename... bindings_t, typename fn_t>
[[nodiscard]] constexpr bool visit_field_by_key(const field_map<owner_t, bindings_t...>& map,
                                                const std::uint64_t key,
                                                fn_t&& fn) {
  bool found = false;
  std::apply(
      [&](const auto&... binding) {
        (([&] {
           using binding_t = std::remove_cvref_t<decltype(binding)>;
           if constexpr (std::invocable<fn_t&, const binding_t&>) {
             if (!found && binding.key == key) {
               std::invoke(fn, binding);
               found = true;
             }
           }
         }()),
         ...);
      },
      map.bindings);
  return found;
}

template <typename... ts>
using type_key_registry = ::wh::internal::type_alias_registry<ts...>;

template <typename... ts>
[[nodiscard]] constexpr auto find_type_key(const std::string_view alias) noexcept
    -> std::optional<type_key> {
  const auto hash = type_key_registry<ts...>::find_hash(alias);
  if (!hash.has_value()) {
    return std::nullopt;
  }
  return type_key{*hash};
}

template <typename... ts>
[[nodiscard]] constexpr std::string_view find_type_alias(const type_key key) noexcept {
  return type_key_registry<ts...>::find_alias(key.value);
}

}  // namespace wh::core
